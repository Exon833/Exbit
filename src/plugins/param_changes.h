// Parameter changes on their way somewhere (to a plugin's processor, its window, its Wine host):
// the latest value of each, found by its id at once. Plugins have thousands of parameters (Serum 2:
// 2,622) and a preset or automation can change them all at once, every buffer: a list searched from
// its start for every change cost millions of comparisons a buffer, on the audio thread.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace plug {

struct ParamValueChange {
    uint32_t id;
    double value;
};

// where each parameter id is in a list: open addressing, at most half full. Each slot is stamped
// with the generation it was filled in, so emptying the index is one count up, not a clearing of
// every slot (the audio thread empties its lists every buffer). It allocates only when it grows:
// reserve(), or a list outgrowing the room it was given (rarely)
class IdIndex {
public:
    static constexpr int32_t NONE = -1;

    // room for `count` ids without allocating
    void reserve(size_t count) {
        size_t want = 16;
        while (want < 2 * count)
            want *= 2;
        if (want <= slots.size())
            return;
        std::vector<Slot> old;
        old.swap(slots);
        slots.assign(want, Slot{});
        bits = 0;
        while (((size_t)1 << bits) < want)
            bits++;
        uint32_t was = gen;
        gen = 1;
        for (const Slot& s : old)
            if (s.gen == was)
                place(s.id, s.at);
    }

    // where id is (NONE: not in it)
    int32_t find(uint32_t id) const {
        if (slots.empty())
            return NONE;
        for (size_t i = home(id);; i = (i + 1) & (slots.size() - 1)) {
            const Slot& s = slots[i];
            if (s.gen != gen)
                return NONE;
            if (s.id == id)
                return s.at;
        }
    }

    // (id isn't in it yet)
    void insert(uint32_t id, int32_t at) {
        if (2 * (count + 1) > slots.size())
            reserve(2 * (count + 1));  // (twice the room: growing is rare)
        place(id, at);
        count++;
    }

    void clear() {
        count = 0;
        if (++gen == 0) {  // (after 4 billion clears: the stamps start again)
            for (Slot& s : slots)
                s.gen = 0;
            gen = 1;
        }
    }

    size_t size() const { return count; }
    size_t room() const { return slots.size() / 2; }

private:
    struct Slot {
        uint32_t id = 0;
        uint32_t gen = 0;  // (the generation it was filled in: an older one is empty)
        int32_t at = 0;
    };
    std::vector<Slot> slots;
    unsigned bits = 0;
    uint32_t gen = 1;
    size_t count = 0;

    // (Fibonacci hashing: ids counted up from 0 and ids that are hashes both spread)
    size_t home(uint32_t id) const { return (size_t)((uint32_t)(id * 2654435769u) >> (32 - bits)); }

    void place(uint32_t id, int32_t at) {
        size_t i = home(id);
        while (slots[i].gen == gen)
            i = (i + 1) & (slots.size() - 1);
        slots[i] = {id, gen, at};
    }
};

// the latest value of each parameter changed, in the order they were first changed
class LatestChanges {
public:
    void reserve(size_t count) {
        list.reserve(count);
        index.reserve(count);
    }

    void set(uint32_t id, double value) {
        int32_t at = index.find(id);
        if (at != IdIndex::NONE) {
            list[(size_t)at].value = value;
            return;
        }
        index.insert(id, (int32_t)list.size());
        list.push_back({id, value});
    }

    bool find(uint32_t id, double& value) const {
        int32_t at = index.find(id);
        if (at == IdIndex::NONE)
            return false;
        value = list[(size_t)at].value;
        return true;
    }

    const std::vector<ParamValueChange>& items() const { return list; }
    bool empty() const { return list.empty(); }
    size_t size() const { return list.size(); }
    // how many it takes without allocating
    size_t room() const { return std::min(list.capacity(), index.room()); }

    // (keeps its room)
    void clear() {
        list.clear();
        index.clear();
    }

    // the first n gone (sent), the others kept in their order (without allocating)
    void dropFirst(size_t n) {
        if (n >= list.size()) {
            clear();
            return;
        }
        list.erase(list.begin(), list.begin() + (std::ptrdiff_t)n);
        index.clear();
        for (size_t i = 0; i < list.size(); i++)
            index.insert(list[i].id, (int32_t)i);
    }

private:
    std::vector<ParamValueChange> list;
    IdIndex index;
};

// LatestChanges between threads: guarded by a mutex held only for a moment (the audio thread takes
// them with try_lock, and adds to them, automation and output parameters, without allocating once
// reserve()d for the plugin's parameters). take() copies the changes out and empties the queue in
// place, so it keeps its room: swapping it away (as before) left it empty-handed after every idle(),
// and the audio thread's next add() allocated, a frame at a time for every automated parameter and
// every meter a plugin reports.
class ChangeQueue {
public:
    static constexpr size_t ROOM = 512;  // (before reserve(): more distinct parameters at once, it grows, rarely)

    ChangeQueue() { changes.reserve(ROOM); }

    void reserve(size_t count) {
        std::lock_guard<std::mutex> hold(lock);
        changes.reserve(count);
    }

    void add(uint32_t id, double value) {
        std::lock_guard<std::mutex> hold(lock);
        changes.set(id, value);
    }

    bool find(uint32_t id, double& value) {
        std::lock_guard<std::mutex> hold(lock);
        return changes.find(id, value);
    }

    // into: the changes, in order (it keeps its own room: no allocation once it has enough).
    // false: none taken (without wait, the queue was busy)
    bool take(std::vector<ParamValueChange>& into, bool wait) {
        std::unique_lock<std::mutex> hold(lock, std::defer_lock);
        if (wait)
            hold.lock();
        else if (!hold.try_lock())
            return false;
        into.assign(changes.items().begin(), changes.items().end());
        changes.clear();
        return true;
    }

    size_t room() {
        std::lock_guard<std::mutex> hold(lock);
        return changes.room();
    }

private:
    std::mutex lock;
    LatestChanges changes;
};

}  // namespace plug
