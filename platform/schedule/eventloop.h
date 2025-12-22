#ifndef _ROP_PLATFORM_EVENTLOOP_H
#define _ROP_PLATFORM_EVENTLOOP_H

namespace RopEventloop {

class IEventLoop {
public:
    
};

class IWatcher {
public:
    virtual ~IWatcher() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
};

}

#endif // _ROP_PLATFORM_EVENTLOOP_H