# Data-Driven Procedural Style (DDPS) - C language coding style

Data-Driven Procedural Style (DDPS) is a C coding style that blends DOD (Data-Oriented Design) with Procedural Programming. It prioritizes cache-friendly memory layouts (specifically Structure of Arrays) and stateless procedural pipelines over object-oriented or pointer-heavy pointer-chasing structures.

---

## 1. philosophy

1. **Separation of Data and Logic**: Data is represented as clean, flat, passive structures. Logic is implemented as stateless procedures operating on that data.
2. **Memory-First Layout**: Design data structures for optimal cache utilization. Prefer **Structure of Arrays (SoA)** over Array of Structures (AoS) for bulk processing.
3. **Stateless Pipelines**: Functions should be pure, linear procedures that process sequential slices of memory. Minimize branching inside performance-critical loops.
4. **No Pointer Chasing**: Avoid linked lists, trees, and deep pointer hierarchies. Use flat arrays, indices, and handles.

---

## 2. Naming Conventions

* **Structs**: `PascalCase` (e.g., `ParticleSystem`).
* **Functions**: `snake_case` (e.g., `particles_update_positions`).
* **Variables and Parameters**: `snake_case` (e.g., `active_count`).
* **Constants and Macros**: `UPPER_SNAKE_CASE` (e.g., `MAX_PARTICLES`).
* **File Names**: `snake_case` matching the subsystem (e.g., `particle_system.h`).

---

## 3. Data Layout Guidelines (SoA)

When designing data structures, group related fields into parallel arrays inside a single managing structure rather than wrapping them in an individual entity structure.

### Bad (AoS)
```c
//ineficient for bulk processing
typedef struct {
    float x, y;
    float vx, vy;
    float lifetime;
    bool active;
} Particle;

typedef struct {
    Particle list[1024];
    int count;
} ParticleSystem;
```

### Good (SoA)
```c
//good for cache utilization and auto-vectorization
typedef struct {
    float x[1024];
    float y[1024];
    float vx[1024];
    float vy[1024];
    float lifetime[1024];
    bool active[1024];
    int count;
} ParticleSystem;
```

---

## 4. Function & Procedure Guidelines

1. **Minimize Scope**: Functions should only receive the specific arrays they need to read or write, rather than the entire system context.
2. **Restrict Pointer Aliasing**: Use the `restrict` keyword on pointer parameters to enable aggressive compiler optimizations and auto-vectorization.
3. **Linear Access Patterns**: Keep memory access sequential. Avoid random access indices inside high-performance loops.

### Example Function Implementation
```c
// procedure receives pointers to the arrays it actually modifies/reads
void particles_update_positions(
    float *restrict x, 
    float *restrict y, 
    const float *restrict vx, 
    const float *restrict vy, 
    const bool *restrict active, 
    int count, 
    float dt
) {
    for (int i = 0; i < count; ++i) {
        if (active[i]) {
            x[i] += vx[i] * dt;
            y[i] += vy[i] * dt;
        }
    }
}
```



## 5. Entity Management

To maintain cache density always keep active items packed at the beginning of the arrays

* **Allocation**: Increment the `count` and initialize fields at the new index.
* **Deallocation (Swap-and-Pop)**: Swap the deactivated element with the last active element in the arrays, then decrement the `count` to maintain a contiguous active block.

### Example: Swap-and-Pop
```c
void particle_system_deactivate(ParticleSystem *sys, int index) {
    if (index < 0 || index >= sys->count) return;

    sys->count--;
    
    // Swap the deactivated element with the last active element
    sys->x[index] = sys->x[sys->count];
    sys->y[index] = sys->y[sys->count];
    sys->vx[index] = sys->vx[sys->count];
    sys->vy[index] = sys->vy[sys->count];
    sys->lifetime[index] = sys->lifetime[sys->count];
    sys->active[index] = sys->active[sys->count];
}
```

## 6. Dynamic Memory Management

In DDPS, individual element allocation (`malloc` for a single entity or struct) is strictly forbidden. It leads to heap fragmentation and pointer chasing. Instead, use bulk allocation patterns.

### 1. Bulk Allocation & Arenas
* Memory must be pre-allocated in large blocks (Arenas or Pools) at startup or subsystem initialization.
* Performance-critical procedures and processing loops must never perform dynamic allocations or deallocations.

### 2. Dynamically Resizable SoA
If the maximum capacity is not known at compile time, manage all parallel arrays inside a single allocation or via synchronized reallocations.

#### Example: Dynamically Resizable SoA
```c
typedef struct {
    float *x;
    float *y;
    float *vx;
    float *vy;
    float *lifetime;
    bool  *active;
    int    count;
    int    capacity;
} DynamicParticleSystem;

// Initialize the entire system in bulk
bool dynamic_particles_init(DynamicParticleSystem *sys, int initial_capacity) {
    sys->x = malloc(sizeof(float) * initial_capacity);
    sys->y = malloc(sizeof(float) * initial_capacity);
    sys->vx = malloc(sizeof(float) * initial_capacity);
    sys->vy = malloc(sizeof(float) * initial_capacity);
    sys->lifetime = malloc(sizeof(float) * initial_capacity);
    sys->active = malloc(sizeof(bool) * initial_capacity);
    
    if (!sys->x || !sys->y || !sys->vx || !sys->vy || !sys->lifetime || !sys->active) {
        // Clean up any successfully allocated buffers
        free(sys->x); free(sys->y); free(sys->vx); free(sys->vy); free(sys->lifetime); free(sys->active);
        return false;
    }
    
    sys->count = 0;
    sys->capacity = initial_capacity;
    return true;
}

// Resize all parallel arrays simultaneously
bool dynamic_particles_reserve(DynamicParticleSystem *sys, int new_capacity) {
    if (new_capacity <= sys->capacity) return true;

    float *new_x = realloc(sys->x, sizeof(float) * new_capacity);
    float *new_y = realloc(sys->y, sizeof(float) * new_capacity);
    float *new_vx = realloc(sys->vx, sizeof(float) * new_capacity);
    float *new_vy = realloc(sys->vy, sizeof(float) * new_capacity);
    float *new_lifetime = realloc(sys->lifetime, sizeof(float) * new_capacity);
    bool *new_active = realloc(sys->active, sizeof(bool) * new_capacity);

    if (!new_x || !new_y || !new_vx || !new_vy || !new_lifetime || !new_active) {
        // Restore successfully reallocated pointers to avoid memory leaks
        if (new_x) sys->x = new_x;
        if (new_y) sys->y = new_y;
        if (new_vx) sys->vx = new_vx;
        if (new_vy) sys->vy = new_vy;
        if (new_lifetime) sys->lifetime = new_lifetime;
        if (new_active) sys->active = new_active;
        return false;
    }

    sys->x = new_x;
    sys->y = new_y;
    sys->vx = new_vx;
    sys->vy = new_vy;
    sys->lifetime = new_lifetime;
    sys->active = new_active;
    sys->capacity = new_capacity;
    return true;
}

// Bulk free when shutting down the subsystem
void dynamic_particles_free(DynamicParticleSystem *sys) {
    free(sys->x);
    free(sys->y);
    free(sys->vx);
    free(sys->vy);
    free(sys->lifetime);
    free(sys->active);
    sys->x = sys->y = sys->vx = sys->vy = sys->lifetime = NULL;
    sys->active = NULL;
    sys->count = 0;
    sys->capacity = 0;
}
```

---

## 7. Multithreading & Parallel Processing

DDPS and SoA memory layouts are exceptionally well-suited for multithreading. Because data is stored in contiguous parallel arrays and processed by stateless procedures, workloads can be easily distributed across multiple CPU cores without complex locks.

### 1. Data Partitioning (Range Splitting)
* Divide the large continuous array `[0, count)` into independent ranges (chunks) for each thread.
* **No Synchronization inside loops**: Because each thread operates on a distinct, non-overlapping index range of the parallel arrays, no mutexes, semaphores, or atomic operations are needed.
* **Avoid False Sharing**: Ensure chunk sizes are large enough (or aligned to 64-byte cache line boundaries) to prevent multiple CPU cores from writing to the same cache line.

### 2. Double Buffering (Read/Write Separation)
If an update procedure requires reading neighboring elements that might be modified by other threads:
* Never read and write to the same array simultaneously across threads.
* Maintain two instances of the arrays: `ReadState` (immutable during the frame) and `WriteState` (mutable).
* Threads read safely from `ReadState` and write to their partitioned ranges in `WriteState`.
* Swap the read/write pointers at the end of the frame/update cycle.

#### Example: Range-Partitioned Multithreaded Update
```c
typedef struct {
    float *x;
    float *y;
    float *vx;
    float *vy;
    bool  *active;
    int    start_index;
    int    end_index;
    float  dt;
} ThreadWorkUnit;

// The thread procedure: processes only its assigned range of the SoA
void *particles_update_range_worker(void *arg) {
    ThreadWorkUnit *work = (ThreadWorkUnit *)arg;
    
    float *restrict x = work->x;
    float *restrict y = work->y;
    const float *restrict vx = work->vx;
    const float *restrict vy = work->vy;
    const bool *restrict active = work->active;
    float dt = work->dt;

    for (int i = work->start_index; i < work->end_index; ++i) {
        if (active[i]) {
            x[i] += vx[i] * dt;
            y[i] += vy[i] * dt;
        }
    }
    
}
```

### 3. Memory-Constrained Multithreading
Double Buffering is highly efficient but doubles memory consumption, which can be prohibitive in memory-constrained environments. If memory is critical, use one of the following strategies to process data in-place safely without full duplication:

#### A. Boundary Halo Buffering (Local Scratchpads)
* Instead of duplicating the entire SoA array, allocate a tiny temporary thread-local or shared scratchpad buffer.
* Copy only the boundary/neighbor elements (halo cells) that are shared between thread range edges into the scratchpad.
* This drops memory overhead from $O(N)$ (duplicating the whole array) to $O(T)$ where $T$ is the number of threads (storing just a few elements at the slice boundaries).

#### B. Independent Phase Split (Barrier Synchronization)
Divide the updates into two distinct execution phases separated by a lightweight thread barrier:
1. **Phase 1 (Parallel Inner Update)**: Each thread processes its internal elements (which do not require reading cross-thread boundary neighbors) concurrently with zero synchronization.
2. **Phase 2 (Synchronized Boundary Update)**: Threads synchronize at a barrier, then cooperatively or sequentially update the shared boundary elements, ensuring safe reads of already updated neighboring values.

#### C. Spatial Tiling & Thread Assignment
For spatial simulations (e.g. physics engines, grids):
* Group elements into spatial grids or tiles.
* Assign threads to non-adjacent tiles (e.g., in a checkerboard pattern).
* Threads process active tiles in parallel without locks because no two active threads share a border.
* Switch active tile groups sequentially to finish the remaining boundary tiles.
