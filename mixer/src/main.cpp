#include <iostream>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <fstream>
#include <limits>
#include "AudioRingBuffer.h"
#include "UDPSocket.h"

const int MAX_AGENTS = 1000;
const int LOGOFF_CHECK_INTERVAL = 1000;

const int MIXER_LISTEN_PORT = 55443;

const int BUFFER_LENGTH_BYTES = 1024;
const int BUFFER_LENGTH_SAMPLES = BUFFER_LENGTH_BYTES / sizeof(int16_t);
const float SAMPLE_RATE = 22050.0;
const float BUFFER_SEND_INTERVAL_USECS = (BUFFER_LENGTH_SAMPLES/SAMPLE_RATE) * 1000000;

const short JITTER_BUFFER_MSECS = 20;
const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_MSECS * (SAMPLE_RATE / 1000.0);

const short RING_BUFFER_FRAMES = 10;
const short RING_BUFFER_SAMPLES = RING_BUFFER_FRAMES * BUFFER_LENGTH_SAMPLES;

const long MAX_SAMPLE_VALUE = std::numeric_limits<int16_t>::max();
const long MIN_SAMPLE_VALUE = std::numeric_limits<int16_t>::min();

char DOMAIN_HOSTNAME[] = "highfidelity.below92.com";
char DOMAIN_IP[100] = "";    //  IP Address will be re-set by lookup on startup
const int DOMAINSERVER_PORT = 40102;

const int MAX_SOURCE_BUFFERS = 20;

sockaddr_in agentAddress;

UDPSocket audioSocket = UDPSocket(MIXER_LISTEN_PORT);

struct AgentList {
    char *address;
    unsigned short port;
    bool active;
    timeval time;
    bool bufferTransmitted;
} agents[MAX_AGENTS];

int numAgents = 0;

AudioRingBuffer *sourceBuffers[MAX_SOURCE_BUFFERS];

double diffclock(timeval *clock1, timeval *clock2)
{
    double diffms = (clock2->tv_sec - clock1->tv_sec) * 1000.0;
    diffms += (clock2->tv_usec - clock1->tv_usec) / 1000.0;   // us to ms
    return diffms;
}

double usecTimestamp(timeval *time, double addedUsecs = 0) {
    return (time->tv_sec * 1000000.0) + time->tv_usec + addedUsecs;
}

void *sendBuffer(void *args)
{
    int sentBytes;
    int currentFrame = 1;
    timeval startTime, sendTime, now;

    int16_t *clientMix = new int16_t[BUFFER_LENGTH_SAMPLES];
    long *masterMix = new long[BUFFER_LENGTH_SAMPLES];

    gettimeofday(&startTime, NULL);

    while (true) {
        sentBytes = 0;

        for (int wb = 0; wb < BUFFER_LENGTH_SAMPLES; wb++) {
            masterMix[wb] = 0;
        }

        gettimeofday(&sendTime, NULL);

        for (int b = 0; b < MAX_SOURCE_BUFFERS; b++) {
            if (sourceBuffers[b]->endOfLastWrite != NULL) {
                if (!sourceBuffers[b]->started 
                && sourceBuffers[b]->diffLastWriteNextOutput() <= BUFFER_LENGTH_SAMPLES + JITTER_BUFFER_SAMPLES) {
                    std::cout << "Held back buffer " << b << ".\n";
                } else if (sourceBuffers[b]->diffLastWriteNextOutput() < BUFFER_LENGTH_SAMPLES) {
                    std::cout << "Buffer " << b << " starved.\n";
                    sourceBuffers[b]->started = false;
                } else {
                    sourceBuffers[b]->started = true;
                    agents[b].bufferTransmitted = true;

                    for (int s =  0; s < BUFFER_LENGTH_SAMPLES; s++) {
                        masterMix[s] += sourceBuffers[b]->nextOutput[s];
                    }

                    sourceBuffers[b]->nextOutput += BUFFER_LENGTH_SAMPLES;

                    if (sourceBuffers[b]->nextOutput >= sourceBuffers[b]->buffer + RING_BUFFER_SAMPLES) {
                        sourceBuffers[b]->nextOutput = sourceBuffers[b]->buffer;
                    }
                }
            }   
        }

        for (int a = 0; a < numAgents; a++) {
            if (diffclock(&agents[a].time, &sendTime) <= LOGOFF_CHECK_INTERVAL) {
                
                int16_t *previousOutput = NULL;
                if (agents[a].bufferTransmitted) {
                    previousOutput = (sourceBuffers[a]->nextOutput == sourceBuffers[a]->buffer) 
                        ? sourceBuffers[a]->buffer + RING_BUFFER_SAMPLES - BUFFER_LENGTH_SAMPLES
                        : sourceBuffers[a]->nextOutput - BUFFER_LENGTH_SAMPLES;
                    agents[a].bufferTransmitted = false;
                }

                for(int as = 0; as < BUFFER_LENGTH_SAMPLES; as++) {
                    long longSample = previousOutput != NULL 
                        ? masterMix[as] - previousOutput[as]
                        : masterMix[as];

                    int16_t shortSample;
                    
                    if (longSample < 0) {
                        shortSample = std::max(longSample, MIN_SAMPLE_VALUE);
                    } else {
                        shortSample = std::min(longSample, MAX_SAMPLE_VALUE);
                    }                 

                    clientMix[as] = shortSample;

                    // std::cout << as << " - CM: " << clientMix[as] << " MM: " << masterMix[as] << "\n";
                    // std::cout << previousOutput - sourceBuffers[a]->buffer << "\n";
                    
                    if (previousOutput != NULL) {
                        // std::cout << "PO: " << previousOutput[as] << "\n";
                    }
                   
                }

                sentBytes = audioSocket.send(agents[a].address, agents[a].port, clientMix, BUFFER_LENGTH_BYTES);
            
                if (sentBytes < BUFFER_LENGTH_BYTES) {
                    std::cout << "Error sending mix packet! " << sentBytes << strerror(errno) << "\n";
                }
            }
        }   

        gettimeofday(&now, NULL);
        
        double usecToSleep = usecTimestamp(&startTime, (currentFrame * BUFFER_SEND_INTERVAL_USECS)) - usecTimestamp(&now);

        if (usecToSleep > 0) {
            usleep(usecToSleep);
        } else {
            std::cout << "NOT SLEEPING!";
        }

        currentFrame++;
    }  

    pthread_exit(0);  
}

int addAgent(sockaddr_in *newAddress, void *audioData) {
    //  Search for agent in list and add if needed
    int is_new = 0;
    int i = 0;
    
    for (i = 0; i < numAgents; i++) {
        if (strcmp(inet_ntoa(newAddress->sin_addr), agents[i].address) == 0
            && ntohs(newAddress->sin_port) == agents[i].port) {
            break;
        }
    }
    
    if ((i == numAgents) || (agents[i].active == false)) {
        is_new = 1;
        
        agents[i].address = new char();
        strcpy(agents[i].address, inet_ntoa(newAddress->sin_addr));
        
        agents[i].bufferTransmitted = false;
    }
    
    
    agents[i].port = ntohs(newAddress->sin_port);
    agents[i].active = true;
    gettimeofday(&agents[i].time, NULL);
    
    if (sourceBuffers[i]->endOfLastWrite == NULL) {
        sourceBuffers[i]->endOfLastWrite = sourceBuffers[i]->buffer;
    } else if (sourceBuffers[i]->diffLastWriteNextOutput() > RING_BUFFER_SAMPLES - BUFFER_LENGTH_SAMPLES) {
        // reset us to started state
        sourceBuffers[i]->endOfLastWrite = sourceBuffers[i]->buffer;
        sourceBuffers[i]->nextOutput = sourceBuffers[i]->buffer;
        sourceBuffers[i]->started = false;
    }
    
    memcpy(sourceBuffers[i]->endOfLastWrite, audioData, BUFFER_LENGTH_BYTES);
    
    sourceBuffers[i]->endOfLastWrite += BUFFER_LENGTH_SAMPLES;
    
    if (sourceBuffers[i]->endOfLastWrite >= sourceBuffers[i]->buffer + RING_BUFFER_SAMPLES) {
        sourceBuffers[i]->endOfLastWrite = sourceBuffers[i]->buffer;
    }
    
    if (i == numAgents) {
        numAgents++;
    }
    
    return is_new;
}

void *reportAliveToDS(void *args) {
    
    timeval lastSend, now;
    char output[100];
   
    while (true) {
        gettimeofday(&lastSend, NULL);
        
        sprintf(output, "%c %f,%f,%f", 'M', 0.f, 0.f, 0.f);
        int packetSize = strlen(output);
        audioSocket.send(DOMAIN_IP, DOMAINSERVER_PORT, output, packetSize);
        
        gettimeofday(&now, NULL);
        
        double usecToSleep = 1000000 - (usecTimestamp(&now) - usecTimestamp(&lastSend));
        
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        } else {
            std::cout << "No sleep required!";
        }
    }    
}

int main(int argc, const char * argv[])
{    
    timeval lastAgentUpdate;
    int receivedBytes = 0;
    
    //  Lookup the IP address of things we have hostnames
    printf("need to look this one up\n");
    struct hostent* pHostInfo;
    if ((pHostInfo = gethostbyname(DOMAIN_HOSTNAME)) != NULL) {
        sockaddr_in tempAddress;
        memcpy(&tempAddress.sin_addr, pHostInfo->h_addr_list[0], pHostInfo->h_length);
        strcpy(DOMAIN_IP, inet_ntoa(tempAddress.sin_addr));
        printf("Domain server found: %s\n", DOMAIN_IP);
        
    } else {
        printf("Failed lookup domain server\n");
    }
    
    // setup the agentSocket to report to domain server
    pthread_t reportAliveThread;
    pthread_create(&reportAliveThread, NULL, reportAliveToDS, NULL);
    

    gettimeofday(&lastAgentUpdate, NULL);

    int16_t packetData[BUFFER_LENGTH_SAMPLES];

    for (int b = 0; b < MAX_SOURCE_BUFFERS; b++) {
        sourceBuffers[b] = new AudioRingBuffer(10 * BUFFER_LENGTH_SAMPLES);
    }

    pthread_t sendBufferThread;
    pthread_create(&sendBufferThread, NULL, sendBuffer, NULL);

    while (true) {
        if(audioSocket.receive(&agentAddress, packetData, &receivedBytes)) {
            
            if (addAgent(&agentAddress, packetData)) {
                std::cout << "Added agent: " <<
                inet_ntoa(agentAddress.sin_addr) << " on " <<
                ntohs(agentAddress.sin_port) << "\n";
            }
        }
    }
    
    pthread_join(reportAliveThread, NULL);
    pthread_join(sendBufferThread, NULL);
    
    return 0;
}

