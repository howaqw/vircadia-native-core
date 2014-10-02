//
//  IceServer.h
//  ice-server/src
//
//  Created by Stephen Birarda on 2014-10-01.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_IceServer_h
#define hifi_IceServer_h

#include <QCoreApplication>

class IceServer : public QCoreApplication {
public:
    IceServer(int argc, char* argv[]);
};

#endif // hifi_IceServer_h