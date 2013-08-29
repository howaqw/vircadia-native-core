//
//  VoxelSendThread.h
//  voxel-server
//
//  Created by Brad Hefta-Gaub on 8/21/13
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//
//  Threaded or non-threaded object for sending voxels to a client
//

#ifndef __voxel_server__VoxelSendThread__
#define __voxel_server__VoxelSendThread__

#include <GenericThread.h>
#include <NetworkPacket.h>
#include <VoxelTree.h>
#include <VoxelNodeBag.h>
#include "VoxelNodeData.h"

/// Threaded processor for sending voxel packets to a single client
class VoxelSendThread : public virtual GenericThread {
public:
    VoxelSendThread(uint16_t nodeID);
protected:
    /// Implements generic processing behavior for this thread.
    virtual bool process();

private:
    uint16_t _nodeID;

    void handlePacketSend(Node* node, VoxelNodeData* nodeData, int& trueBytesSent, int& truePacketsSent);
    void deepestLevelVoxelDistributor(Node* node, VoxelNodeData* nodeData, bool viewFrustumChanged);
    
    unsigned char _tempOutputBuffer[MAX_VOXEL_PACKET_SIZE];
};

#endif // __voxel_server__VoxelSendThread__
