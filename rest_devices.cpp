/*
 * Copyright (c) 2013-2019 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include <QString>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QVariantMap>
#include <QProcess>
#include "de_web_plugin.h"
#include "de_web_plugin_private.h"
#include "device_descriptions.h"
#include "json.h"
#include "rest_devices.h"
#include "utils/utils.h"

RestDevices::RestDevices(QObject *parent) :
    QObject(parent)
{
    plugin = qobject_cast<DeRestPluginPrivate*>(parent);
    Q_ASSERT(plugin);
}

/*! Devices REST API broker.
    \param req - request data
    \param rsp - response data
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int RestDevices::handleApi(const ApiRequest &req, ApiResponse &rsp)
{
    if (req.path[2] != QLatin1String("devices"))
    {
        return REQ_NOT_HANDLED;
    }

    // GET /api/<apikey>/devices
    if ((req.path.size() == 3) && (req.hdr.method() == QLatin1String("GET")))
    {
        return getAllDevices(req, rsp);
    }
    // GET /api/<apikey>/devices/<uniqueid>
    else if ((req.path.size() == 4) && (req.hdr.method() == QLatin1String("GET")))
    {
        return getDevice(req, rsp);
    }
    // PUT /api/<apikey>/devices/<uniqueid>/ddf/reload
    else if (req.path.size() == 6 && req.hdr.method() == QLatin1String("PUT") && req.path[4] == QLatin1String("ddf") && req.path[5] == QLatin1String("reload"))
    {
        return putDeviceReloadDDF(req, rsp);
    }
    // PUT /api/<apikey>/devices/<uniqueid>/installcode
    else if ((req.path.size() == 5) && (req.hdr.method() == QLatin1String("PUT")) && (req.path[4] == QLatin1String("installcode")))
    {
        return putDeviceInstallCode(req, rsp);
    }


    return REQ_NOT_HANDLED;
}

/*! Deletes a Sensor as a side effect it will be removed from the REST API
    and a ZDP reset will be send if possible.
 */
bool deleteSensor(Sensor *sensor, DeRestPluginPrivate *plugin)
{
    if (sensor && plugin && sensor->deletedState() == Sensor::StateNormal)
    {
        sensor->setDeletedState(Sensor::StateDeleted);
        sensor->setNeedSaveDatabase(true);
        sensor->setResetRetryCount(10);

        plugin->enqueueEvent(Event(sensor->prefix(), REventDeleted, sensor->id()));
        return true;
    }

    return false;
}

/*! Deletes a LightNode as a side effect it will be removed from the REST API
    and a ZDP reset will be send if possible.
 */
bool deleteLight(LightNode *lightNode, DeRestPluginPrivate *plugin)
{
    if (lightNode && plugin && lightNode->state() == LightNode::StateNormal)
    {
        lightNode->setState(LightNode::StateDeleted);
        lightNode->setResetRetryCount(10);
        lightNode->setNeedSaveDatabase(true);

        // delete all group membership from light (todo this is messy)
        for (auto &group : lightNode->groups())
        {
            //delete Light from all scenes.
            plugin->deleteLightFromScenes(lightNode->id(), group.id);

            //delete Light from all groups
            group.actions &= ~GroupInfo::ActionAddToGroup;
            group.actions |= GroupInfo::ActionRemoveFromGroup;
            if (group.state != GroupInfo::StateNotInGroup)
            {
                group.state = GroupInfo::StateNotInGroup;
            }
        }

        plugin->enqueueEvent(Event(lightNode->prefix(), REventDeleted, lightNode->id()));
        return true;
    }

    return false;
}

/*! Deletes all resources related to a device from the REST API.
 */
bool RestDevices::deleteDevice(quint64 extAddr)
{
    int count = 0;

    for (auto &sensor : plugin->sensors)
    {
        if (sensor.address().ext() == extAddr && deleteSensor(&sensor, plugin))
        {
            count++;
        }
    }

    for (auto &lightNode : plugin->nodes)
    {
        if (lightNode.address().ext() == extAddr && deleteLight(&lightNode, plugin))
        {
            count++;
        }
    }

    if (count > 0)
    {
        plugin->queSaveDb(DB_SENSORS | DB_LIGHTS | DB_GROUPS | DB_SCENES, DB_SHORT_SAVE_DELAY);
    }

    // delete device entry, regardless if REST resources exists
    plugin->deleteDeviceDb(generateUniqueId(extAddr, 0, 0));

    plugin->enqueueEvent(Event(RDevices, REventDeleted, 0, extAddr));

    return count > 0;
}

void RestDevices::handleEvent(const Event &event)
{
    if (event.resource() == RDevices && event.what() == REventDeleted)
    {
        DEV_RemoveDevice(plugin->m_devices, event.deviceKey());
    }
}

/*! GET /api/<apikey>/devices
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int RestDevices::getAllDevices(const ApiRequest &req, ApiResponse &rsp)
{
    Q_UNUSED(req)

    rsp.httpStatus = HttpStatusOk;

    for (const auto &d : plugin->m_devices)
    {
        Q_ASSERT(d);
        rsp.list.push_back(d->item(RAttrUniqueId)->toString());
    }

    if (rsp.list.isEmpty())
    {
        rsp.str = QLatin1String("[]"); // return empty list
    }
    return REQ_READY_SEND;
}

/*! GET /api/<apikey>/devices/<uniqueid>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED

    Unstable API to experiment: don't use in production!
 */
int RestDevices::getDevice(const ApiRequest &req, ApiResponse &rsp)
{
    DBG_Assert(req.path.size() == 4);

    rsp.httpStatus = HttpStatusOk;

    auto uniqueId = req.path.at(3);
    uniqueId.remove(QLatin1Char(':'));

    bool ok;
    const auto deviceKey = uniqueId.toULongLong(&ok, 16);

    Device *device = DEV_GetDevice(plugin->m_devices, deviceKey);

    rsp.httpStatus = device ? HttpStatusOk : HttpStatusNotFound;

    if (!device)
    {
        return REQ_READY_SEND;
    }

    const DeviceDescription ddf = plugin->deviceDescriptions->get(device);

    if (ddf.isValid())
    {
        rsp.map["productid"] = ddf.product;
    }

    QVariantList subDevices;

    for (const auto &sub : device->subDevices())
    {
        QVariantMap map;

        for (int i = 0; i < sub->itemCount(); i++)
        {
            auto *item = sub->itemForIndex(i);
            Q_ASSERT(item);

            if (item->descriptor().suffix == RStateLastUpdated ||
                item->descriptor().suffix == RAttrId)
            {
                continue;
            }

            if (!item->isPublic())
            {
                continue;
            }

            const auto ls = QString(QLatin1String(item->descriptor().suffix)).split(QLatin1Char('/'));

            if (ls.size() == 2)
            {
                if (item->descriptor().suffix == RAttrLastSeen || item->descriptor().suffix == RAttrLastAnnounced ||
                    item->descriptor().suffix == RAttrManufacturerName || item->descriptor().suffix == RAttrModelId ||
                    item->descriptor().suffix == RAttrSwVersion || item->descriptor().suffix == RAttrName)
                {
                    if (!rsp.map.contains(ls.at(1)))
                    {
                        rsp.map[ls.at(1)] = item->toString(); // top level attribute
                    }
                }
                else if (ls.at(0) == QLatin1String("attr"))
                {
                    map[ls.at(1)] = item->toVariant(); // sub device top level attribute
                }
                else
                {
                    QVariantMap m2;
                    if (map.contains(ls.at(0)))
                    {
                        m2 = map[ls.at(0)].toMap();
                    }

                    QVariantMap itemMap;

                    itemMap[QLatin1String("value")] = item->toVariant();

                    QDateTime dt = item->lastChanged().isValid() ? item->lastChanged() : item->lastSet();
                    // UTC in msec resolution
                    dt.setOffsetFromUtc(0);
                    itemMap[QLatin1String("lastupdated")] = dt.toString(QLatin1String("yyyy-MM-ddTHH:mm:ssZ"));


                    m2[ls.at(1)] = itemMap;
                    map[ls.at(0)] = m2;
                }
            }
        }

        subDevices.push_back(map);
    }

    rsp.map["uniqueid"] = device->item(RAttrUniqueId)->toString();
    rsp.map["subdevices"] = subDevices;

    return REQ_READY_SEND;
}

/*! PUT /api/<apikey>/devices/<uniqueid>/installcode
    \return REQ_READY_SEND
            REQ_NOT_HANDLED

    Adds an Zigbee 3.0 Install Code for a device to let it securely join.
    Unstable API to experiment: don't use in production!
 */
int RestDevices::putDeviceInstallCode(const ApiRequest &req, ApiResponse &rsp)
{
    DBG_Assert(req.path.size() == 5);

    bool ok;
    const QString &uniqueid = req.path[3];

    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();

    if (!ok || map.isEmpty())
    {
        rsp.list.append(plugin->errorToMap(ERR_INVALID_JSON, QString("/devices/%1/installcode").arg(uniqueid), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    // installcode
    if (map.contains("installcode"))
    {
        QString installCode = map["installcode"].toString().trimmed();

        if (map["installcode"].type() == QVariant::String && !installCode.isEmpty())
        {
            // TODO process install code

            // MAC: f8f005fffff2b37a
            // IC: 07E2EE0C820EFE0C0C21742E0A037C07
            // CRC-16 7AC5

            QProcess cli;
            cli.start("hashing-cli", QStringList() << "-i" << installCode);
            if (!cli.waitForStarted(2000))
            {
                rsp.list.append(plugin->errorToMap(ERR_INTERNAL_ERROR, QString("/devices"), QString("internal error, %1, occured").arg(cli.error())));
                rsp.httpStatus = HttpStatusServiceUnavailable;
                return REQ_READY_SEND;
            }

            if (!cli.waitForFinished(2000))
            {
                rsp.list.append(plugin->errorToMap(ERR_INTERNAL_ERROR, QString("/devices"), QString("internal error, %1, occured").arg(cli.error())));
                rsp.httpStatus = HttpStatusServiceUnavailable;
                return REQ_READY_SEND;
            }

            QByteArray mmoHash;
            while (!cli.atEnd())
            {
                const QByteArray result = cli.readLine();
                if (result.contains("Hash Result:"))
                {
                    const auto ls = result.split(':');
                    DBG_Assert(ls.size() == 2);
                    if (ls.size() == 2)
                    {
                        mmoHash = ls[1].trimmed();
                        break;
                    }
                }
            }

            if (mmoHash.isEmpty())
            {
                rsp.list.append(plugin->errorToMap(ERR_INTERNAL_ERROR, QLatin1String("/devices"), QLatin1String("internal error, failed to calc mmo hash, occured")));
                rsp.httpStatus = HttpStatusServiceUnavailable;
                return REQ_READY_SEND;
            }

#if DECONZ_LIB_VERSION >= 0x010B00
            QVariantMap m;
            m["mac"] = uniqueid.toULongLong(&ok, 16);
            m["key"] = mmoHash;
            if (ok && mmoHash.size() == 32)
            {
                ok = deCONZ::ApsController::instance()->setParameter(deCONZ::ParamLinkKey, m);
            }
#endif
            QVariantMap rspItem;
            QVariantMap rspItemState;
            rspItemState["installcode"] = installCode;
            rspItemState["mmohash"] = mmoHash;
            rspItem["success"] = rspItemState;
            rsp.list.append(rspItem);
            rsp.httpStatus = HttpStatusOk;
            return REQ_READY_SEND;
        }
        else
        {
            rsp.list.append(plugin->errorToMap(ERR_INVALID_VALUE, QString("/devices"), QString("invalid value, %1, for parameter, installcode").arg(installCode)));
            rsp.httpStatus = HttpStatusBadRequest;
        }
    }
    else
    {
        rsp.list.append(plugin->errorToMap(ERR_MISSING_PARAMETER, QString("/devices/%1/installcode").arg(uniqueid), QString("missing parameters in body")));
        rsp.httpStatus = HttpStatusBadRequest;
    }

    return REQ_READY_SEND;
}

int RestDevices::putDeviceReloadDDF(const ApiRequest &req, ApiResponse &rsp)
{
    DBG_Assert(req.path.size() == 6);

    rsp.httpStatus = HttpStatusOk;

    auto uniqueId = req.path.at(3);
    uniqueId.remove(QLatin1Char(':'));

    bool ok = false;
    const auto deviceKey = uniqueId.toULongLong(&ok, 16);

    if (ok)
    {
        emit eventNotify(Event(RDevices, REventDDFReload, 0, deviceKey));

        QVariantMap rspItem;
        QVariantMap rspItemState;
        rspItemState["reload"] = req.path.at(3);
        rspItem["success"] = rspItemState;
        rsp.list.append(rspItem);
        rsp.httpStatus = HttpStatusOk;
    }
    else
    {
        // TODO
    }

    return REQ_READY_SEND;
}
