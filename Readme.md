🌸 Lotus

Lotus is a very simple, header-only, templated asset manager for C++.

**Features:**

* **Single header** (lotus.hpp)

* **Templated** – no polymorphism required

*  **Callbacks** – you define how to load/unload

* **Handles** – safe references that keep resources alive while in use

* **Thread safe** – registry access guarded by std::mutex

🛠️ API Overview

```cpp
// Create registry (provide load + unload callbacks)
lotus::resource_registry<T> registry(load_fn, unload_fn);

// Request resource by name (loads if missing)
auto handle = lotus::get("id", registry);

// Register resource manually (already loaded object)
lotus::reg("id", pointer_to_T, registry);

// Reload all resources 
//(requires no on-going read on registry resources)
lotus::reload_registry(registry);

// Unload all resources
//(requires no on-going read on registry resources)
lotus::unload_registry(registry);

// Handle methods
handle.good();   // check if resource is ready
handle->...;     // access the resource
```
