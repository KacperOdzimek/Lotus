#pragma once

#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <unordered_map>

//=================
// Forwards

namespace lotus {
    // reference to resource
    // resource lifetime is bound to it's handles; when last handle expires the resource is unloaded
    template<class resource_type>
    struct resource_handle;

    // registry of resources of given type
    template<class resource_type>
    struct resource_registry;

    // called when resource requested by "get" function is not loaded
    template<class resource_type>
    using resource_request_callback = void(*)(const char*, resource_registry<resource_type>&);

    // called when the resource is no longer in use
    template<class resource_type>
    using resource_unload_callback = void(*)(resource_type*);

    // returns handle to a resource in registry
    // thread safe
    template<class resource_type> 
    resource_handle<resource_type> get(const char*, resource_registry<resource_type>&);

    // register resource in registry under given name
    // after this call the registry shall be in charge of resource deletion
    // thread safe
    template<class resource_type>
    void reg(const char*, resource_type*, resource_registry<resource_type>&);

    // unloads and loads all currently loaded resources
    // requieres none of the resources is read at the time
    template<class resource_type>
    void reload_registry(resource_registry<resource_type>&);

    // unloads all loaded resources
    // requieres none of the resources is read at the time
    template<class resource_type>
    void unload_registry(resource_registry<resource_type>&);

    // returns whether the resource under handle is ready to use
    // bool resource_handle<resource_type>::good();

    // reads the resource
    // use only after checking good()
    //const resource_type* resource_handle<resource_type>::operator->() const 
}

//=================
// Resource Registry

template<class resource_type>
struct lotus::resource_registry {
private:
    using shared = typename lotus::resource_handle<resource_type>::shared;
    using states = typename lotus::resource_handle<resource_type>::states;

    resource_request_callback<resource_type> rrc;
    resource_unload_callback<resource_type>  ruc;

    std::mutex mutex;

    std::unordered_map<
        std::string,
        shared*
    > reg;

    friend resource_handle<resource_type> lotus::get<resource_type>(
        const char*, resource_registry<resource_type>&
    );

    friend void lotus::reg<resource_type>(
        const char*, resource_type*, resource_registry<resource_type>&
    );

    friend void reload_registry<resource_type>(resource_registry<resource_type>&);
    friend void unload_registry<resource_type>(resource_registry<resource_type>&);

    friend lotus::resource_handle<resource_type>;

    //call under mutex
    shared* find_or_create_shared(const char* name) {
        auto itr = reg.find(name); 
    
        if (itr == reg.end()) {
            auto shr = new shared;

            shr->state.store(states::unloaded);
            shr->count.store(0);
            shr->object   = nullptr;
            shr->registry = this;
            
            itr = reg.insert({name, shr}).first;
        }

        return itr->second;
    }

public:
    resource_registry(
        resource_request_callback<resource_type> _rrc,
        resource_unload_callback<resource_type>  _ruc
    ) : rrc(_rrc), ruc(_ruc) {};
};

//=================
// Resource Handle

template<class resource_type>
struct lotus::resource_handle {
private:
    enum class states {
        loaded,
        unloaded,
        waiting_load,
    };

    struct shared {
        std::atomic<states>                         state;
        std::atomic<unsigned int>                   count;
        resource_type*                              object;
        lotus::resource_registry<resource_type>*    registry;
    };

    shared* shr;

    friend resource_handle<resource_type> lotus::get<resource_type>(
        const char*, resource_registry<resource_type>&
    );

    friend void lotus::reg<resource_type>(
        const char*, resource_type*, resource_registry<resource_type>&
    );

    friend void reload_registry<resource_type>(resource_registry<resource_type>&);
    friend void unload_registry<resource_type>(resource_registry<resource_type>&);

    friend lotus::resource_registry<resource_type>;

    resource_handle(shared* _shr) : shr(_shr) {
        if (shr) shr->count.fetch_add(1, std::memory_order_acq_rel);
    };

public:
    resource_handle() : shr(nullptr) {}  
    
    resource_handle(const resource_handle<resource_type>& other) : shr(other.shr) {                
        if (shr) shr->count.fetch_add(1, std::memory_order_acq_rel);
    }

    resource_handle<resource_type>& operator=(const resource_handle<resource_type>& other) {                
        if (shr == other.shr) return *this;

        this->~resource_handle();
        new(this) resource_handle(other.shr);

        return *this;
    }

    ~resource_handle() {                
        if (shr && shr->count.fetch_sub(1, std::memory_order_acq_rel) == 1 && shr->state.load() == states::loaded) {
            shr->state.store(states::unloaded);
            shr->registry->ruc(shr->object);
        }
        shr = nullptr;
    } 

    // returns whether the resource under handle is ready to use
    const bool good() const {
        return shr->state.load() == states::loaded;
    }

    // reads the resource
    // use only after checking good()
    const resource_type* operator->() const {
        return shr->object;
    }
};

//=================
// Functions

template<class resource_type>
lotus::resource_handle<resource_type> lotus::get(
    const char*                         name, 
    resource_registry<resource_type>&   reg
) {
    using shared = typename lotus::resource_handle<resource_type>::shared;
    using states = typename lotus::resource_handle<resource_type>::states;

    std::lock_guard<std::mutex> lock(reg.mutex);
    auto shr = reg.find_or_create_shared(name);
    lock.~lock_guard();

    if (shr->state.load() == states::unloaded) {
        reg.rrc(name, reg);
    }

    return lotus::resource_handle<resource_type>{shr};
}

template<class resource_type>
void lotus::reg(
    const char*                         name, 
    resource_type*                      object, 
    resource_registry<resource_type>&   reg
) {
    using shared = typename lotus::resource_handle<resource_type>::shared;
    using states = typename lotus::resource_handle<resource_type>::states;

    std::lock_guard<std::mutex> lock(reg.mutex);
    auto shr = reg.find_or_create_shared(name);
    lock.~lock_guard();

    shr->object = object;
    shr->state.store(states::loaded);
}


template<class resource_type>
void lotus::reload_registry(resource_registry<resource_type>& reg) {
    using shared = typename lotus::resource_handle<resource_type>::shared;
    using states = typename lotus::resource_handle<resource_type>::states;

    std::lock_guard<std::mutex> lock(reg.mutex);

    //cache and call after unlocking the lock to avoid deadlock with reg func
    std::vector<std::string> to_load;

    for (auto& p : reg.reg) {
        auto& shr = p.second;
        
        if (shr->state.load() == states::loaded) {
            shr->state.store(states::unloaded);
            reg.ruc(shr->object);
            to_load.push_back(p.first);
        }
    }

    lock.~lock_guard();
    for (auto& res : to_load) reg.rrc(res.c_str(), reg);
}

template<class resource_type>
void lotus::unload_registry(resource_registry<resource_type>& reg) {
    using shared = typename lotus::resource_handle<resource_type>::shared;
    using states = typename lotus::resource_handle<resource_type>::states;

    std::lock_guard<std::mutex> lock(reg.mutex);

    for (auto& p : reg.reg) {
        auto& shr = p.second;
        
        if (shr->state.load() == states::loaded) {
            shr->state.store(states::unloaded);
            reg.ruc(shr->object);
        }
    }
}
