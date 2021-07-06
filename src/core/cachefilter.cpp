/*
* Copyright (c) 2012-2020 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/


#include <string>
#include <algorithm>
#include "VSHelper4.h"
#include "cachefilter.h"

VSCache::CacheAction VSCache::recommendSize() {
    int total = hits + nearMiss + farMiss;
 
    if (total == 0) {
#ifdef VS_CACHE_DEBUG
        fprintf(stderr, "Cache (%p) stats (clear): total: %d, far miss: %d, near miss: %d, hits: %d, size: %d\n", (void *)this, total, farMiss, nearMiss, hits, maxSize);
#endif
        return CacheAction::Clear;
    }

    if (total < 30) {
#ifdef VS_CACHE_DEBUG
        fprintf(stderr, "Cache (%p) stats (keep low total): total: %d, far miss: %d, near miss: %d, hits: %d, size: %d\n", (void *)this, total, farMiss, nearMiss, hits, maxSize);
#endif
        return CacheAction::NoChange; // not enough requests to know what to do so keep it this way
    }

    bool shrink = (nearMiss == 0 && hits == 0); // shrink if there were no hits or even close to hittin
    bool grow = ((nearMiss * 20) >= total); // grow if 5% or more are near misses
#ifdef VS_CACHE_DEBUG
    fprintf(stderr, "Cache (%p) stats (%s): total: %d, far miss: %d, near miss: %d, hits: %d, size: %d\n", (void *)this, shrink ? "shrink" : (grow ? "grow" : "keep"), total, farMiss, nearMiss, hits, maxSize);
#endif

    if (grow) { // growing the cache would be beneficial
        clearStats();
        return CacheAction::Grow;
    } else if (shrink) { // probably a linear scan, no reason to waste space here
        clearStats();
        return CacheAction::Shrink;
    } else {
        clearStats();
        return CacheAction::NoChange; // probably fine the way it is
    }
}

inline VSCache::VSCache(int maxSize, int maxHistorySize, bool fixedSize)
    : maxSize(maxSize), maxHistorySize(maxHistorySize), fixedSize(fixedSize) {
    clear();
}

inline PVSFrameRef VSCache::object(const int key) {
    return this->relink(key);
}

inline bool VSCache::remove(const int key) {
    auto i = hash.find(key);

    if (i == hash.end()) {
        return false;
    } else {
        unlink(i->second);
        return true;
    }
}


bool VSCache::insert(const int akey, const PVSFrameRef &aobject) {
    assert(aobject);
    assert(akey >= 0);
    remove(akey);
    auto i = hash.insert(std::make_pair(akey, Node(akey, aobject)));
    currentSize++;
    Node *n = &i.first->second;

    if (first)
        first->prevNode = n;

    n->nextNode = first;
    first = n;

    if (!last)
        last = first;

    trim(maxSize, maxHistorySize);

    return true;
}


void VSCache::trim(int max, int maxHistory) {
    // first adjust the number of cached frames and extra history length
    while (currentSize > max) {
        if (!weakpoint)
            weakpoint = last;
        else
            weakpoint = weakpoint->prevNode;

        if (weakpoint)
            weakpoint->frame.reset();

        currentSize--;
        historySize++;
    }

    // remove history until the tail is small enough
    while (last && historySize > maxHistory) {
        unlink(*last);
    }
}

void VSCache::adjustSize(bool needMemory) {
    if (!fixedSize) {
        if (!needMemory) {
            switch (recommendSize()) {
            case VSCache::CacheAction::Clear:
                clear();
                setMaxFrames(std::max(getMaxFrames() - 2, 0));
                break;
            case VSCache::CacheAction::Grow:
                setMaxFrames(getMaxFrames() + 2);
                break;
            case VSCache::CacheAction::Shrink:
                setMaxFrames(std::max(getMaxFrames() - 1, 0));
                break;
            default:;
            }
        } else {
            switch (recommendSize()) {
            case VSCache::CacheAction::Clear:
                clear();
                setMaxFrames(std::max(getMaxFrames() - 2, 0));
                break;
            case VSCache::CacheAction::Shrink:
                setMaxFrames(std::max(getMaxFrames() - 2, 0));
                break;
            case VSCache::CacheAction::NoChange:
                if (getMaxFrames() <= 1)
                    clear();
                setMaxFrames(std::max(getMaxFrames() - 1, 1));
                break;
            default:;
            }
        }
    }
}

// controls how many frames beyond the number of threads is a good margin to catch bigger temporal radius filters that are out of order, just a guess
static const int extraFrames = 7;

static const VSFrame *VS_CC cacheGetframe(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    CacheInstance *c = static_cast<CacheInstance *>(instanceData);

    intptr_t *fd = (intptr_t *)frameData;

    if (activationReason == arInitial) {
        PVSFrameRef f = c->cache.object(n);

        if (f) {
            f->add_ref();
            return f.get();
        }

        if (c->makeLinear && n != c->lastN + 1 && n > c->lastN && n < c->lastN + c->numThreads + extraFrames) {
            for (int i = c->lastN + 1; i <= n; i++)
                vsapi->requestFrameFilter(i, c->clip, frameCtx);
            *fd = c->lastN;
        } else {
            vsapi->requestFrameFilter(n, c->clip, frameCtx);
            *fd = -2;
        }

        c->lastN = n;
        return nullptr;
    } else if (activationReason == arAllFramesReady) {
        if (*fd >= -1) {
            for (intptr_t i = *fd + 1; i < n; i++) {
                const VSFrame *r = vsapi->getFrameFilter((int)i, c->clip, frameCtx);
                c->cache.insert((int)i, const_cast<VSFrame *>(r));
            }
        }

        const VSFrame *r = vsapi->getFrameFilter(n, c->clip, frameCtx);
        c->cache.insert(n, PVSFrameRef(const_cast<VSFrame *>(r), true));
        return r;
    }

    return nullptr;
}

static void VS_CC cacheFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    CacheInstance *c = static_cast<CacheInstance *>(instanceData);
    c->removeCache();
    vsapi->freeNode(c->clip);
    delete c;
}

static void VS_CC createCacheFilter(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    static std::atomic<size_t> cacheId(1);
    VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    int err;
    bool fixed = !!vsapi->mapGetInt(in, "fixed", 0, &err);
    CacheInstance *c = new CacheInstance(node, core, fixed);
    VSCoreInfo ci;
    vsapi->getCoreInfo(core, &ci);
    c->numThreads = ci.numThreads;
    c->makeLinear = !!vsapi->mapGetInt(in, "make_linear", 0, &err);

    int size = vsapi->mapGetIntSaturated(in, "size", 0, &err);

    if (!err && size > 0)
        c->cache.setMaxFrames(size);
    else if (c->makeLinear)
        c->cache.setMaxFrames(std::max((c->numThreads + extraFrames) * 2, 20 + c->numThreads));
    else
        c->cache.setMaxFrames(20);

    if (userData)
        vsapi->createAudioFilter(out, ("AudioCache" + std::to_string(cacheId++)).c_str(), vsapi->getAudioInfo(node), cacheGetframe, cacheFree, c->makeLinear ? fmUnorderedLinear : fmUnordered, nfNoCache, c, core);
    else
        vsapi->createVideoFilter(out, ("VideoCache" + std::to_string(cacheId++)).c_str(), vsapi->getVideoInfo(node), cacheGetframe, cacheFree, c->makeLinear ? fmUnorderedLinear : fmUnordered, nfNoCache, c, core);

    VSNode *self = vsapi->mapGetNode(out, "clip", 0, nullptr);
    c->addCache(self);
    vsapi->freeNode(self);
}

void VS_CC cacheInitialize(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->registerFunction("Cache", "clip:vnode;size:int:opt;fixed:int:opt;make_linear:int:opt;", "clip:vnode;", createCacheFilter, nullptr, plugin);
    vspapi->registerFunction("VideoCache", "clip:vnode;size:int:opt;fixed:int:opt;make_linear:int:opt;", "clip:vnode;", createCacheFilter, nullptr, plugin);
    vspapi->registerFunction("AudioCache", "clip:anode;size:int:opt;fixed:int:opt;make_linear:int:opt;", "clip:anode;", createCacheFilter, (void *)1, plugin);
}
