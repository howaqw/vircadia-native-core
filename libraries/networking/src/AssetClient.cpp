//
//  AssetClient.cpp
//  libraries/networking/src
//
//  Created by Ryan Huffman on 2015/07/21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetClient.h"

#include <cstdint>

#include <QtCore/QBuffer>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtScript/QScriptEngine>
#include <QtNetwork/QNetworkDiskCache>

#include "AssetRequest.h"
#include "AssetUpload.h"
#include "AssetUtils.h"
#include "MappingRequest.h"
#include "NetworkAccessManager.h"
#include "NetworkLogging.h"
#include "NodeList.h"
#include "PacketReceiver.h"
#include "ResourceCache.h"

MessageID AssetClient::_currentID = 0;

void MappingRequest::start() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "start", Qt::AutoConnection);
        return;
    }
    doStart();
};

GetMappingRequest::GetMappingRequest(const AssetPath& path) : _path(path) {
};

void GetMappingRequest::doStart() {

    auto assetClient = DependencyManager::get<AssetClient>();

    // Check cache
    auto it = assetClient->_mappingCache.constFind(_path);
    if (it != assetClient->_mappingCache.constEnd()) {
        _hash = it.value();
        emit finished(this);
        return;
    }

    assetClient->getAssetMapping(_path, [this, assetClient](bool responseReceived, AssetServerError error, QSharedPointer<ReceivedMessage> message) {
        if (!responseReceived) {
            _error = NetworkError;
        } else {
            switch (error) {
                case AssetServerError::NoError:
                    _error = NoError;
                    break;
                case AssetServerError::AssetNotFound:
                    _error = NotFound;
                    break;
                default:
                    _error = UnknownError;
                    break;
            }
        }

        if (!_error) {
            _hash = message->read(SHA256_HASH_LENGTH).toHex();
            assetClient->_mappingCache[_path] = _hash;
        }
        emit finished(this);
    });
};

GetAllMappingsRequest::GetAllMappingsRequest() {
};

void GetAllMappingsRequest::doStart() {
    auto assetClient = DependencyManager::get<AssetClient>();
    assetClient->getAllAssetMappings([this, assetClient](bool responseReceived, AssetServerError error, QSharedPointer<ReceivedMessage> message) {
        if (!responseReceived) {
            _error = NetworkError;
        } else {
            switch (error) {
                case AssetServerError::NoError:
                    _error = NoError;
                    break;
                default:
                    _error = UnknownError;
                    break;
            }
        }


        if (!error) {
            int numberOfMappings;
            message->readPrimitive(&numberOfMappings);
            assetClient->_mappingCache.clear();
            for (auto i = 0; i < numberOfMappings; ++i) {
                auto path = message->readString();
                auto hash = message->read(SHA256_HASH_LENGTH).toHex();
                _mappings[path] = hash;
                assetClient->_mappingCache[path] = hash;
            }
        }
        emit finished(this);
    });
};

SetMappingRequest::SetMappingRequest(const AssetPath& path, const AssetHash& hash) : _path(path), _hash(hash) {
};

void SetMappingRequest::doStart() {
    auto assetClient = DependencyManager::get<AssetClient>();
    assetClient->setAssetMapping(_path, _hash, [this, assetClient](bool responseReceived, AssetServerError error, QSharedPointer<ReceivedMessage> message) {
        if (!responseReceived) {
            _error = NetworkError;
        } else {
            switch (error) {
                case AssetServerError::NoError:
                    _error = NoError;
                    break;
                case AssetServerError::PermissionDenied:
                    _error = PermissionDenied;
                    break;
                default:
                    _error = UnknownError;
                    break;
            }
        }

        if (!error) {
            assetClient->_mappingCache[_path] = _hash;
        }
        emit finished(this);
    });
};

DeleteMappingsRequest::DeleteMappingsRequest(const AssetPathList& paths) : _paths(paths) {
};

void DeleteMappingsRequest::doStart() {
    auto assetClient = DependencyManager::get<AssetClient>();
    assetClient->deleteAssetMappings(_paths, [this, assetClient](bool responseReceived, AssetServerError error, QSharedPointer<ReceivedMessage> message) {
        if (!responseReceived) {
            _error = NetworkError;
        } else {
            switch (error) {
                case AssetServerError::NoError:
                    _error = NoError;
                    break;
                case AssetServerError::PermissionDenied:
                    _error = PermissionDenied;
                    break;
                default:
                    _error = UnknownError;
                    break;
            }
        }

        if (!error) {
            // enumerate the paths and remove them from the cache
            for (auto& path : _paths) {
                assetClient->_mappingCache.remove(path);
            }
        }
        emit finished(this);
    });
};

AssetClient::AssetClient() {
    
    setCustomDeleter([](Dependency* dependency){
        static_cast<AssetClient*>(dependency)->deleteLater();
    });
    
    auto nodeList = DependencyManager::get<NodeList>();
    auto& packetReceiver = nodeList->getPacketReceiver();

    packetReceiver.registerListener(PacketType::AssetMappingOperationReply, this, "handleAssetMappingOperationReply");
    packetReceiver.registerListener(PacketType::AssetGetInfoReply, this, "handleAssetGetInfoReply");
    packetReceiver.registerListener(PacketType::AssetGetReply, this, "handleAssetGetReply", true);
    packetReceiver.registerListener(PacketType::AssetUploadReply, this, "handleAssetUploadReply");

    connect(nodeList.data(), &LimitedNodeList::nodeKilled, this, &AssetClient::handleNodeKilled);
}

void AssetClient::init() {
    Q_ASSERT(QThread::currentThread() == thread());

    // Setup disk cache if not already
    auto& networkAccessManager = NetworkAccessManager::getInstance();
    if (!networkAccessManager.cache()) {
        QString cachePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
        cachePath = !cachePath.isEmpty() ? cachePath : "interfaceCache";

        QNetworkDiskCache* cache = new QNetworkDiskCache();
        cache->setMaximumCacheSize(MAXIMUM_CACHE_SIZE);
        cache->setCacheDirectory(cachePath);
        networkAccessManager.setCache(cache);
        qDebug() << "ResourceManager disk cache setup at" << cachePath
                 << "(size:" << MAXIMUM_CACHE_SIZE / BYTES_PER_GIGABYTES << "GB)";
    }
}


void AssetClient::cacheInfoRequest(QObject* reciever, QString slot) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "cacheInfoRequest", Qt::QueuedConnection,
                                  Q_ARG(QObject*, reciever), Q_ARG(QString, slot));
        return;
    }


    if (auto* cache = qobject_cast<QNetworkDiskCache*>(NetworkAccessManager::getInstance().cache())) {
        QMetaObject::invokeMethod(reciever, slot.toStdString().data(), Qt::QueuedConnection,
                                  Q_ARG(QString, cache->cacheDirectory()),
                                  Q_ARG(qint64, cache->cacheSize()),
                                  Q_ARG(qint64, cache->maximumCacheSize()));
    } else {
        qCWarning(asset_client) << "No disk cache to get info from.";
    }
}

void AssetClient::clearCache() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "clearCache", Qt::QueuedConnection);
        return;
    }

    _mappingCache.clear();

    if (auto cache = NetworkAccessManager::getInstance().cache()) {
        qDebug() << "AssetClient::clearCache(): Clearing disk cache.";
        cache->clear();
    } else {
        qCWarning(asset_client) << "No disk cache to clear.";
    }
}

void AssetClient::handleAssetMappingOperationReply(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    MessageID messageID;
    message->readPrimitive(&messageID);
    
    AssetServerError error;
    message->readPrimitive(&error);

    // Check if we have any pending requests for this node
    auto messageMapIt = _pendingMappingRequests.find(senderNode);
    if (messageMapIt != _pendingMappingRequests.end()) {

        // Found the node, get the MessageID -> Callback map
        auto& messageCallbackMap = messageMapIt->second;

        // Check if we have this pending request
        auto requestIt = messageCallbackMap.find(messageID);
        if (requestIt != messageCallbackMap.end()) {
            auto callback = requestIt->second;
            callback(true, error, message);
            messageCallbackMap.erase(requestIt);
        }

        // Although the messageCallbackMap may now be empty, we won't delete the node until we have disconnected from
        // it to avoid constantly creating/deleting the map on subsequent requests.
    }
}

bool haveAssetServer() {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
    
    if (!assetServer) {
        qCWarning(asset_client) << "Could not complete AssetClient operation "
            << "since you are not currently connected to an asset-server.";
        return false;
    }
    
    return true;
}
GetMappingRequest* AssetClient::createGetMappingRequest(const AssetPath& path) {
    return new GetMappingRequest(path);
}

GetAllMappingsRequest* AssetClient::createGetAllMappingsRequest() {
    return new GetAllMappingsRequest();
}

DeleteMappingsRequest* AssetClient::createDeleteMappingsRequest(const AssetPathList& paths) {
    return new DeleteMappingsRequest(paths);
}

SetMappingRequest* AssetClient::createSetMappingRequest(const AssetPath& path, const AssetHash& hash) {
    return new SetMappingRequest(path, hash);
}

RenameMappingRequest* AssetClient::createRenameMappingRequest(const AssetPath& oldPath, const AssetPath& newPath) {
    return new RenameMappingRequest(oldPath, newPath);
}

AssetRequest* AssetClient::createRequest(const AssetHash& hash) {
    if (hash.length() != SHA256_HASH_HEX_LENGTH) {
        qCWarning(asset_client) << "Invalid hash size";
        return nullptr;
    }

    if (haveAssetServer()) {
        auto request = new AssetRequest(hash);
        
        // Move to the AssetClient thread in case we are not currently on that thread (which will usually be the case)
        request->moveToThread(thread());
        
        return request;
    } else {
        return nullptr;
    }
}

AssetUpload* AssetClient::createUpload(const QString& filename) {
    
    if (haveAssetServer()) {
        auto upload = new AssetUpload(filename);
        
        upload->moveToThread(thread());
        
        return upload;
    } else {
        return nullptr;
    }
}

AssetUpload* AssetClient::createUpload(const QByteArray& data) {
    if (haveAssetServer()) {
        auto upload = new AssetUpload(data);
        
        upload->moveToThread(thread());
        
        return upload;
    } else {
        return nullptr;
    }
}

bool AssetClient::getAsset(const QString& hash, DataOffset start, DataOffset end,
                           ReceivedAssetCallback callback, ProgressCallback progressCallback) {
    if (hash.length() != SHA256_HASH_HEX_LENGTH) {
        qCWarning(asset_client) << "Invalid hash size";
        return false;
    }

    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);

    if (assetServer) {
        
        auto messageID = ++_currentID;
        
        auto payloadSize = sizeof(messageID) + SHA256_HASH_LENGTH + sizeof(start) + sizeof(end);
        auto packet = NLPacket::create(PacketType::AssetGet, payloadSize, true);
        
        qCDebug(asset_client) << "Requesting data from" << start << "to" << end << "of" << hash << "from asset-server.";
        
        packet->writePrimitive(messageID);

        packet->write(QByteArray::fromHex(hash.toLatin1()));

        packet->writePrimitive(start);
        packet->writePrimitive(end);

        nodeList->sendPacket(std::move(packet), *assetServer);

        _pendingRequests[assetServer][messageID] = { callback, progressCallback };

        return true;
    }

    return false;
}

bool AssetClient::getAssetInfo(const QString& hash, GetInfoCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);

    if (assetServer) {
        auto messageID = ++_currentID;
        
        auto payloadSize = sizeof(messageID) + SHA256_HASH_LENGTH;
        auto packet = NLPacket::create(PacketType::AssetGetInfo, payloadSize, true);
        
        packet->writePrimitive(messageID);
        packet->write(QByteArray::fromHex(hash.toLatin1()));

        nodeList->sendPacket(std::move(packet), *assetServer);

        _pendingInfoRequests[assetServer][messageID] = callback;

        return true;
    }

    return false;
}

void AssetClient::handleAssetGetInfoReply(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    MessageID messageID;
    message->readPrimitive(&messageID);
    auto assetHash = message->read(SHA256_HASH_LENGTH);
    
    AssetServerError error;
    message->readPrimitive(&error);

    AssetInfo info { assetHash.toHex(), 0 };

    if (error == AssetServerError::NoError) {
        message->readPrimitive(&info.size);
    }

    // Check if we have any pending requests for this node
    auto messageMapIt = _pendingInfoRequests.find(senderNode);
    if (messageMapIt != _pendingInfoRequests.end()) {

        // Found the node, get the MessageID -> Callback map
        auto& messageCallbackMap = messageMapIt->second;

        // Check if we have this pending request
        auto requestIt = messageCallbackMap.find(messageID);
        if (requestIt != messageCallbackMap.end()) {
            auto callback = requestIt->second;
            callback(true, error, info);
            messageCallbackMap.erase(requestIt);
        }

        // Although the messageCallbackMap may now be empty, we won't delete the node until we have disconnected from
        // it to avoid constantly creating/deleting the map on subsequent requests.
    }
}

void AssetClient::handleAssetGetReply(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    auto assetHash = message->read(SHA256_HASH_LENGTH);
    qCDebug(asset_client) << "Got reply for asset: " << assetHash.toHex();

    MessageID messageID;
    message->readHeadPrimitive(&messageID);

    AssetServerError error;
    message->readHeadPrimitive(&error);

    DataOffset length = 0;
    if (!error) {
        message->readHeadPrimitive(&length);
    } else {
        qCWarning(asset_client) << "Failure getting asset: " << error;
    }

    // Check if we have any pending requests for this node
    auto messageMapIt = _pendingRequests.find(senderNode);
    if (messageMapIt != _pendingRequests.end()) {

        // Found the node, get the MessageID -> Callback map
        auto& messageCallbackMap = messageMapIt->second;

        // Check if we have this pending request
        auto requestIt = messageCallbackMap.find(messageID);
        if (requestIt != messageCallbackMap.end()) {
            auto& callbacks = requestIt->second;

            if (message->isComplete()) {
                callbacks.completeCallback(true, error, message->readAll());
            } else {
                connect(message.data(), &ReceivedMessage::progress, this, [this, length, message, callbacks]() {
                    callbacks.progressCallback(message->getSize(), length);
                });
                connect(message.data(), &ReceivedMessage::completed, this, [this, message, error, callbacks]() {
                    if (message->failed()) {
                        callbacks.completeCallback(false, AssetServerError::NoError, QByteArray());
                    } else {
                        callbacks.completeCallback(true, error, message->readAll());
                    }
                });
            }
            messageCallbackMap.erase(requestIt);
        }

        // Although the messageCallbackMap may now be empty, we won't delete the node until we have disconnected from
        // it to avoid constantly creating/deleting the map on subsequent requests.
    }
}

bool AssetClient::getAssetMapping(const AssetPath& path, MappingOperationCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);

    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetMappingOperation, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        packetList->writePrimitive(AssetMappingOperationType::Get);

        packetList->writeString(path);

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingMappingRequests[assetServer][messageID] = callback;

        return true;
    }

    return false;
}

bool AssetClient::getAllAssetMappings(MappingOperationCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
    
    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetMappingOperation, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        packetList->writePrimitive(AssetMappingOperationType::GetAll);

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingMappingRequests[assetServer][messageID] = callback;

        return true;
    }

    return false;
}

bool AssetClient::deleteAssetMappings(const AssetPathList& paths, MappingOperationCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
    
    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetMappingOperation, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        packetList->writePrimitive(AssetMappingOperationType::Delete);

        packetList->writePrimitive(int(paths.size()));

        for (auto& path: paths) {
            packetList->writeString(path);
        }

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingMappingRequests[assetServer][messageID] = callback;

        return true;
    }

    return false;
}

bool AssetClient::setAssetMapping(const QString& path, const AssetHash& hash, MappingOperationCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
    
    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetMappingOperation, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        packetList->writePrimitive(AssetMappingOperationType::Set);

        packetList->writeString(path);
        packetList->write(QByteArray::fromHex(hash.toUtf8()));

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingMappingRequests[assetServer][messageID] = callback;

        return true;
    }

    return false;
}

bool AssetClient::renameAssetMapping(const AssetPath& oldPath, const AssetPath& newPath, MappingOperationCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);

    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetMappingOperation, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        packetList->writePrimitive(AssetMappingOperationType::Rename);

        packetList->writeString(oldPath);
        packetList->writeString(newPath);

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingMappingRequests[assetServer][messageID] = callback;

        return true;

    }

    return false;
}

bool AssetClient::uploadAsset(const QByteArray& data, UploadResultCallback callback) {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
    
    if (assetServer) {
        auto packetList = NLPacketList::create(PacketType::AssetUpload, QByteArray(), true, true);

        auto messageID = ++_currentID;
        packetList->writePrimitive(messageID);

        uint64_t size = data.length();
        packetList->writePrimitive(size);
        packetList->write(data.constData(), size);

        nodeList->sendPacketList(std::move(packetList), *assetServer);

        _pendingUploads[assetServer][messageID] = callback;

        return true;
    }
    return false;
}

void AssetClient::handleAssetUploadReply(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    MessageID messageID;
    message->readPrimitive(&messageID);

    AssetServerError error;
    message->readPrimitive(&error);

    QString hashString;

    if (error) {
        qCWarning(asset_client) << "Error uploading file to asset server";
    } else {
        auto hash = message->read(SHA256_HASH_LENGTH);
        hashString = hash.toHex();
        
        qCDebug(asset_client) << "Successfully uploaded asset to asset-server - SHA256 hash is " << hashString;
    }

    // Check if we have any pending requests for this node
    auto messageMapIt = _pendingUploads.find(senderNode);
    if (messageMapIt != _pendingUploads.end()) {

        // Found the node, get the MessageID -> Callback map
        auto& messageCallbackMap = messageMapIt->second;

        // Check if we have this pending request
        auto requestIt = messageCallbackMap.find(messageID);
        if (requestIt != messageCallbackMap.end()) {
            auto callback = requestIt->second;
            callback(true, error, hashString);
            messageCallbackMap.erase(requestIt);
        }

        // Although the messageCallbackMap may now be empty, we won't delete the node until we have disconnected from
        // it to avoid constantly creating/deleting the map on subsequent requests.
    }
}

void AssetClient::handleNodeKilled(SharedNodePointer node) {
    if (node->getType() != NodeType::AssetServer) {
        return;
    }

    {
        auto messageMapIt = _pendingRequests.find(node);
        if (messageMapIt != _pendingRequests.end()) {
            for (const auto& value : messageMapIt->second) {
                value.second.completeCallback(false, AssetServerError::NoError, QByteArray());
            }
            messageMapIt->second.clear();
        }
    }

    {
        auto messageMapIt = _pendingInfoRequests.find(node);
        if (messageMapIt != _pendingInfoRequests.end()) {
            AssetInfo info { "", 0 };
            for (const auto& value : messageMapIt->second) {
                value.second(false, AssetServerError::NoError, info);
            }
            messageMapIt->second.clear();
        }
    }

    {
        auto messageMapIt = _pendingUploads.find(node);
        if (messageMapIt != _pendingUploads.end()) {
            for (const auto& value : messageMapIt->second) {
                value.second(false, AssetServerError::NoError, "");
            }
            messageMapIt->second.clear();
        }
    }

    {
        auto messageMapIt = _pendingMappingRequests.find(node);
        if (messageMapIt != _pendingMappingRequests.end()) {
            for (const auto& value : messageMapIt->second) {
                value.second(false, AssetServerError::NoError, QSharedPointer<ReceivedMessage>());
            }
            messageMapIt->second.clear();
        }
    }

    _mappingCache.clear();
}

void AssetScriptingInterface::uploadData(QString data, QScriptValue callback) {
    QByteArray dataByteArray = data.toUtf8();
    auto upload = DependencyManager::get<AssetClient>()->createUpload(dataByteArray);
    if (!upload) {
        qCWarning(asset_client) << "Error uploading file to asset server";
        return;
    }

    QObject::connect(upload, &AssetUpload::finished, this, [this, callback](AssetUpload* upload, const QString& hash) mutable {
        if (callback.isFunction()) {
            QString url = "atp://" + hash;
            QScriptValueList args { url };
            callback.call(_engine->currentContext()->thisObject(), args);
        }
    });
    upload->start();
}

AssetScriptingInterface::AssetScriptingInterface(QScriptEngine* engine) :
    _engine(engine)
{
}

void AssetScriptingInterface::downloadData(QString urlString, QScriptValue callback) {
    const QString ATP_SCHEME { "atp://" };

    if (!urlString.startsWith(ATP_SCHEME)) {
        return;
    }

    // Make request to atp
    auto path = urlString.right(urlString.length() - ATP_SCHEME.length());
    auto parts = path.split(".", QString::SkipEmptyParts);
    auto hash = parts.length() > 0 ? parts[0] : "";

    if (hash.length() != SHA256_HASH_HEX_LENGTH) {
        return;
    }

    auto assetClient = DependencyManager::get<AssetClient>();
    auto assetRequest = assetClient->createRequest(hash);

    if (!assetRequest) {
        return;
    }

    _pendingRequests << assetRequest;

    connect(assetRequest, &AssetRequest::finished, this, [this, callback](AssetRequest* request) mutable {
        Q_ASSERT(request->getState() == AssetRequest::Finished);

        if (request->getError() == AssetRequest::Error::NoError) {
            if (callback.isFunction()) {
                QString data = QString::fromUtf8(request->getData());
                QScriptValueList args { data };
                callback.call(_engine->currentContext()->thisObject(), args);
            }
        }

        request->deleteLater();
        _pendingRequests.remove(request);
    });

    assetRequest->start();
}

static int standardItemModelMetaTypeId = qRegisterMetaType<AssetMappingModel*>("AssetMappingModel*");

AssetMappingsScriptingInterface::AssetMappingsScriptingInterface(QJSEngine* engine) :
    _engine(engine),
    _assetMappingModel(this)
{
}

void AssetMappingsScriptingInterface::setMapping(QString path, QString hash, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createSetMappingRequest(path, hash);

    connect(request, &SetMappingRequest::finished, this, [this, callback](SetMappingRequest* request) mutable {
        QJSValueList args { uint8_t(request->getError()) };

        callback.call(args);

        request->deleteLater();
         
    });

    request->start();
}

void AssetMappingsScriptingInterface::getMapping(QString path, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetMappingRequest(path);

    connect(request, &GetMappingRequest::finished, this, [this, callback](GetMappingRequest* request) mutable {
        QJSValueList args { uint8_t(request->getError()), request->getHash() };

        callback.call(args);

        request->deleteLater();
         
    });

    request->start();
}

void AssetMappingsScriptingInterface::deleteMappings(QStringList paths, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createDeleteMappingsRequest(paths);

    connect(request, &DeleteMappingsRequest::finished, this, [this, callback](DeleteMappingsRequest* request) mutable {
        QJSValueList args { uint8_t(request->getError()) };

        callback.call(args);

        request->deleteLater();
         
    });

    request->start();
}

void AssetMappingsScriptingInterface::getAllMappings(QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetAllMappingsRequest();

    connect(request, &GetAllMappingsRequest::finished, this, [this, callback](GetAllMappingsRequest* request) mutable {
        auto mappings = request->getMappings();
        auto map = callback.engine()->newObject();

        for (auto& kv : mappings ) {
            map.setProperty(kv.first, kv.second);
        }

        QJSValueList args { uint8_t(request->getError()), map };

        callback.call(args);

        request->deleteLater();
         
    });

    request->start();
}

AssetMappingModel::AssetMappingModel(QObject* parent) {
}

void AssetMappingModel::refresh() {
    qDebug() << "Refreshing asset mapping model";
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetAllMappingsRequest();

    connect(request, &GetAllMappingsRequest::finished, this, [this](GetAllMappingsRequest* request) mutable {
        qDebug() << "Got response";
        auto mappings = request->getMappings();
//        clear();
        for (auto& mapping : mappings) {
            auto& path = mapping.first;
            auto parts = path.split("/");
            auto length = parts.length();

            QString prefix = parts[0];

            QStandardItem* lastItem = nullptr;

            auto it = _pathToItemMap.find(prefix);
            if (it == _pathToItemMap.end()) {
                lastItem = new QStandardItem(parts[0]);
                _pathToItemMap[prefix] = lastItem;
                appendRow(lastItem);
            } else {
                lastItem = it.value();
            }

            if (length > 1) {
                for (int i = 1; i < length; ++i) {
                    prefix += "/" + parts[i];

                    auto it = _pathToItemMap.find(prefix);
                    if (it == _pathToItemMap.end()) {
                        qDebug() << "prefix not found: " << prefix;
                        auto item = new QStandardItem(parts[i]);
                        lastItem->setChild(lastItem->rowCount(), 0, item);
                        lastItem = item;
                        _pathToItemMap[prefix] = lastItem;
                    } else {
                        lastItem = it.value();
                    }
                }
            }

            Q_ASSERT(prefix == path);
        }
    });

    request->start();
}


// QModelIndex AssetMappingModel::index(int row, int column, const QModelIndex& parent) const {
//     if (row < 0 || column < 0) {
//         return QModelIndex();
//     }

//     if (parent.isValid()) {
//         auto item = static_cast<AssetMappingItem*>(parent.internalPointer());
//         return createIndex(row, column, )
//     }
//     return createIndex(row, column, getFolderNodes(
//         static_cast<AssetMappingItem*>(getTreeNodeFromIndex(parent))).at(row));
// }

// QModelIndex AssetMappingModel::parent(const QModelIndex& child) const {
//     AssetMappingItem* parent = (static_cast<AssetMappingItem*>(child.internalPointer()))->getParent();
//     if (!parent) {
//         return QModelIndex();
//     }
//     AssetMappingItem* grandParent = parent->getParent();
//     int row = getFolderNodes(grandParent).indexOf(parent);
//     return createIndex(row, 0, parent);
// }

// QVariant AssetMappingModel::data(const QModelIndex& index, int role) const {
//     TreeNodeBase* node = getTreeNodeFromIndex(index);
//     if (!node) {
//         return QVariant();
//     }
//     return QVariant();
// }

// int AssetMappingModel::rowCount(const QModelIndex& parent) const {
//     return 1;
// }

// int AssetMappingModel::columnCount(const QModelIndex& parent) const {
//     return 1;
// }
