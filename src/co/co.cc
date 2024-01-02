#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "co/stl.h"
#include "sched.h"

#ifndef _WIN32
#ifdef __linux__
#include <sys/syscall.h>  // for SYS_xxx definitions
#include <time.h>         // for clock_gettime
#include <unistd.h>       // for syscall()

#else
#include <sys/time.h>  // for gettimeofday
#endif
#endif

namespace co {
namespace xx {

// typedef std::mutex mutex_t;
// typedef std::condition_variable cv_t;
// typedef std::unique_lock<std::mutex> mutex_guard_t;

#ifdef __linux__
#ifndef SYS_gettid
#define SYS_gettid __NR_gettid
#endif
uint32_t thread_id() { return syscall(SYS_gettid); }

#else /* for mac, bsd.. */
uint32_t thread_id() {
    uint64_t x;
    pthread_threadid_np(0, &x);
    return (uint32_t)x;
}
#endif

class mutex_impl {
  public:
    struct queue {
        struct _memb : co::clink {
            size_t size;
            uint8_t rx;
            uint8_t wx;
            void* q[];
        };
        static constexpr int N = (128 - sizeof(_memb)) / sizeof(void*);

        _memb* _make_memb() {
            // TODO: alines
            _memb* m =
                (_memb*)::malloc(sizeof(_memb) + N * sizeof(void*));  //, L1_CACHE_LINE_SIZE);
            m->size = 0;
            m->rx = 0;
            m->wx = 0;
            return m;
        }

        queue() noexcept : _m(0) {}

        ~queue() {
            for (auto h = _q.front(); h;) {
                const auto m = (_memb*)h;
                h = h->next;
                ::free(m);
            }
        }

        size_t size() const noexcept { return _m ? _m->size : 0; }
        bool empty() const noexcept { return this->size() == 0; }

        void push_back(void* x) {
            _memb* m = (_memb*)_q.back();
            if (!m || m->wx == N) {
                m = this->_make_memb();
                _q.push_back(m);
            }
            m->q[m->wx++] = x;
            ++_m->size;
        }

        void* pop_front() {
            void* x = 0;
            if (_m && _m->rx < _m->wx) {
                x = _m->q[_m->rx++];
                --_m->size;
                if (_m->rx == _m->wx) {
                    _m->rx = _m->wx = 0;
                    if (_q.back() != _m) {
                        _memb* const m = (_memb*)_q.pop_front();
                        _m->size = m->size;
                        ::free(m);
                    }
                }
            }
            return x;
        }

        union {
            _memb* _m;
            co::clist _q;
        };
    };

    mutex_impl() : _m(), _cv(), _refn(1), _lock(0) {}
    ~mutex_impl() = default;  // {}

    void lock();
    void unlock();
    bool try_lock();

    void ref() { _refn.fetch_add(1, std::memory_order_relaxed); }
    uint32_t unref() { return --_refn; }

  private:
    std::mutex _m;
    std::condition_variable _cv;
    queue _wq;
    std::atomic_uint32_t _refn;
    uint8_t _lock;
};

inline bool mutex_impl::try_lock() {
    std::unique_lock<std::mutex> g(_m);
    return _lock ? false : (_lock = 1);
}

void mutex_impl::lock() {
    const auto sched = xx::gSched;
    if (sched) { /* in coroutine */
        _m.lock();
        if (!_lock) {
            _lock = 1;
            _m.unlock();
        } else {
            Coroutine* const co = sched->running();
            _wq.push_back(co);
            _m.unlock();
            sched->yield();
        }

    } else { /* non-coroutine */

        std::unique_lock<std::mutex> g(_m);
        if (!_lock) {
            _lock = 1;
        } else {
            _wq.push_back(nullptr);
            for (;;) {
                _cv.wait(g);
                if (_lock == 2) {
                    _lock = 1;
                    break;
                }
            }
        }
    }
}

void mutex_impl::unlock() {
    _m.lock();
    if (_wq.empty()) {
        _lock = 0;
        _m.unlock();
    } else {
        Coroutine* const co = (Coroutine*)_wq.pop_front();
        if (co) {
            _m.unlock();
            co->sched->add_ready_task(co);
        } else {
            _lock = 2;
            _m.unlock();
            _cv.notify_one();
        }
    }
}

class event_impl {
  public:
    event_impl(bool m, bool s, uint32_t wg = 0)
        : _m(), _cv(), _wt(0), _sn(0), _refn(1), _wg(wg), _signaled(s), _manual_reset(m) {}
    ~event_impl() = default;  // {}

    bool wait(uint32_t ms);
    void signal();
    void reset();

    inline void ref() noexcept { _refn.fetch_add(1, std::memory_order_relaxed); }
    inline uint32_t unref() noexcept { return --_refn; }
    inline std::atomic_uint32_t& wg() noexcept { return _wg; }

  private:
    // xx::mutex _m;
    // xx::cv_t _cv;
    std::mutex _m;
    std::condition_variable _cv;
    co::clist _wc;
    uint32_t _wt;
    uint32_t _sn;
    std::atomic_uint32_t _refn;
    std::atomic_uint32_t _wg;  // for wait group
    bool _signaled;
    const bool _manual_reset;
};

bool event_impl::wait(uint32_t ms) {
    const auto sched = gSched;
    if (sched) { /* in coroutine */
        Coroutine* co = sched->running();
        {
            std::unique_lock<std::mutex> g(_m);
            if (_signaled) {
                if (!_manual_reset) _signaled = false;
                return true;
            }
            if (ms == 0) return false;

            waitx_t* x = 0;
            while (!_wc.empty()) {
                waitx_t* const w = (waitx_t*)_wc.front();
                if (w->state != st_timeout) break;
                _wc.pop_front();
                !x ? (void)(x = w) : ::free(w);
            }
            x ? (void)(x->state = st_wait) : (void)(x = make_waitx(co));
            co->waitx = x;
            _wc.push_back(x);
        }

        if (ms != (uint32_t)-1) sched->add_timer(ms);
        sched->yield();
        if (!sched->timeout()) ::free(co->waitx);
        co->waitx = nullptr;
        return !sched->timeout();

    } else { /* not in coroutine */

        std::unique_lock<std::mutex> g(_m);
        if (_signaled) {
            if (!_manual_reset) _signaled = false;
            return true;
        }
        if (ms == 0) return false;

        const uint32_t sn = _sn;
        ++_wt;
        if (ms != (uint32_t)-1) {
            const auto r =
                _cv.wait_for(g, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
            if (!r && sn == _sn) {
                assert(_wt > 0);
                --_wt;
            }
            return r;
        } else {
            // xx::cv_wait(&_cv, _m.native_handle());
            _cv.wait(g);
            return true;
        }
    }
}

void event_impl::signal() {
    co::clink* h = 0;
    {
        bool has_wt = false, has_wc = false;

        std::unique_lock<std::mutex> g(_m);
        if (_wt > 0) {
            _wt = 0;
            has_wt = true;
        }

        if (!_wc.empty()) {
            h = _wc.front();
            _wc.clear();
            if (!has_wt) {
                do {
                    waitx_t* const w = (waitx_t*)h;
                    h = h->next;
                    decltype(w->state)::value_type state(st_wait);
                    if (w->state.compare_exchange_strong(state, st_ready, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
                        has_wc = true;
                        w->co->sched->add_ready_task(w->co);
                        break;
                    } else { /* timeout */
                        ::free(w);
                    }
                } while (h);
            }
        }

        if (has_wt || has_wc) {
            if (_signaled && !_manual_reset) _signaled = false;
            if (has_wt) {
                ++_sn;
                _cv.notify_all();
            }
        } else {
            if (!_signaled) _signaled = true;
        }
    }

    while (h) {
        waitx_t* const w = (waitx_t*)h;
        h = h->next;
        decltype(w->state)::value_type state(st_wait);
        if (w->state.compare_exchange_strong(state, st_ready, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            w->co->sched->add_ready_task(w->co);
        } else { /* timeout */
            ::free(w);
        }
    }
}

inline void event_impl::reset() {
    std::unique_lock<std::mutex> g(_m);
    _signaled = false;
}

class sync_event_impl {
  public:
    explicit sync_event_impl(bool m, bool s) : _wt(0), _sn(0), _signaled(s), _manual_reset(m) {}

    ~sync_event_impl() = default;

    void wait() {
        std::unique_lock<std::mutex> g(_m);
        if (_signaled) {
            if (!_manual_reset) _signaled = false;
            return;
        }
        ++_wt;
        _cv.wait(g);
    }

    bool wait(uint32_t ms) {
        std::unique_lock<std::mutex> g(_m);
        if (_signaled) {
            if (!_manual_reset) _signaled = false;
            return true;
        }
        if (ms == 0) return false;

        const uint32_t sn = _sn;
        ++_wt;
        const bool r = _cv.wait_for(g, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
        if (!r && sn == _sn) {
            assert(_wt > 0);
            --_wt;
        }
        return r;
    }

    void signal() {
        std::unique_lock<std::mutex> g(_m);
        if (_wt > 0) {
            _wt = 0;
            if (_signaled && !_manual_reset) _signaled = false;
            ++_sn;
            _cv.notify_all();
        } else {
            if (!_signaled) _signaled = true;
        }
    }

    void reset() {
        std::unique_lock<std::mutex> g(_m);
        _signaled = false;
    }

  private:
    std::mutex _m;
    std::condition_variable _cv;
    uint32_t _wt;
    uint32_t _sn;
    bool _signaled;
    const bool _manual_reset;
};

static thread_local bool g_done = false;

class pipe_impl {
  public:
    pipe_impl(uint32_t buf_size, uint32_t blk_size, uint32_t ms, pipe::C&& c, pipe::D&& d)
        : _buf_size(buf_size),
          _blk_size(blk_size),
          _ms(ms),
          _c(std::move(c)),
          _d(std::move(d)),
          _m(),
          _cv(),
          _rx(0),
          _wx(0),
          _refn(1),
          _full(0),
          _closed(0) {
        _buf = (char*)::malloc(_buf_size);
    }

    ~pipe_impl() { ::free(_buf); }

    void read(void* p);
    void write(void* p, int v);
    bool done() const { return g_done; }
    void close();
    bool is_closed() const { return _closed.load(std::memory_order_relaxed); }

    void ref() { _refn.fetch_add(1, std::memory_order_relaxed); }
    uint32_t unref() { return --_refn; }

    struct waitx : co::clink {
        Coroutine* co;
        union {
            std::atomic_uint8_t state;
            struct {
                std::atomic_uint8_t state;
                uint8_t done;  // 1: ok, 2: channel closed
                uint8_t v;     // 0: cp, 1: mv, 2: need destruct the object in buf
            } x;
            void* dummy;
        };
        void* buf;
        size_t len;  // total length of the memory
    };

    waitx* create_waitx(Coroutine* co, void* buf) {
        waitx* w;
        if (co && gSched->on_stack(buf)) {
            w = (waitx*)::malloc(sizeof(waitx) + _blk_size);
            w->buf = (char*)w + sizeof(waitx);
            w->len = sizeof(waitx) + _blk_size;
        } else {
            w = (waitx*)::malloc(sizeof(waitx));
            w->buf = buf;
            w->len = sizeof(waitx);
        }
        w->co = co;
        w->state = st_wait;
        w->x.done = 0;
        return w;
    }

  private:
    void _read_block(void* p);
    void _write_block(void* p, int v);

  private:
    char* _buf;          // buffer
    uint32_t _buf_size;  // buffer size
    uint32_t _blk_size;  // block size
    uint32_t _ms;        // timeout in milliseconds
    xx::pipe::C _c;
    xx::pipe::D _d;

    std::mutex _m;
    std::condition_variable _cv;
    co::clist _wq;
    uint32_t _rx;  // read pos
    uint32_t _wx;  // write pos
    std::atomic_uint32_t _refn;
    uint8_t _full;
    std::atomic_uint8_t _closed;
};

inline void pipe_impl::_read_block(void* p) {
    _d(p);
    _c(p, _buf + _rx, 1);
    _d(_buf + _rx);
    _rx += _blk_size;
    if (_rx == _buf_size) _rx = 0;
}

inline void pipe_impl::_write_block(void* p, int v) {
    _c(_buf + _wx, p, v);
    _wx += _blk_size;
    if (_wx == _buf_size) _wx = 0;
}

void pipe_impl::read(void* p) {
    auto sched = gSched;
    _m.lock();

    // buffer is neither empty nor full
    if (_rx != _wx) {
        this->_read_block(p);
        _m.unlock();
        goto done;
    }

    // buffer is full
    if (_full) {
        this->_read_block(p);

        while (!_wq.empty()) {
            waitx* w = (waitx*)_wq.pop_front();  // wait for write
            decltype(w->state)::value_type state(st_wait);
            if (_ms == (uint32_t)-1 ||
                w->state.compare_exchange_strong(state, st_ready, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                this->_write_block(w->buf, w->x.v & 1);
                if (w->x.v & 2) _d(w->buf);
                w->x.done = 1;
                if (w->co) {
                    _m.unlock();
                    w->co->sched->add_ready_task(w->co);
                } else {
                    _cv.notify_all();
                    _m.unlock();
                }
                goto done;

            } else { /* timeout */
                if (w->x.v & 2) _d(w->buf);
                ::free(w);
            }
        }

        _full = 0;
        _m.unlock();
        goto done;
    }

    // buffer is empty
    if (this->is_closed()) {
        _m.unlock();
        goto enod;
    }
    if (sched) {
        auto co = sched->running();
        waitx* w = this->create_waitx(co, p);
        w->x.v = (w->buf != p ? 0 : 2);
        _wq.push_back(w);
        _m.unlock();

        co->waitx = (waitx_t*)w;
        if (_ms != (uint32_t)-1) sched->add_timer(_ms);
        sched->yield();

        co->waitx = 0;
        if (!sched->timeout()) {
            if (w->x.done == 1) {
                if (w->buf != p) {
                    _d(p);
                    _c(p, w->buf, 1);  // mv
                    _d(w->buf);
                }
                ::free(w);
                goto done;
            }

            assert(w->x.done == 2);  // channel closed
            ::free(w);
            goto enod;
        }
        goto enod;

    } else {
        bool r = true;
        waitx* w = this->create_waitx(nullptr, p);
        _wq.push_back(w);

        std::unique_lock<std::mutex> g(_m, std::adopt_lock);
        for (;;) {
            if (_ms == (uint32_t)-1) {
                _cv.wait(g);
            } else {
                r = _cv.wait_for(g, std::chrono::milliseconds(_ms)) == std::cv_status::no_timeout;
            }
            decltype(w->state)::value_type state(st_wait);
            if (r || !w->state.compare_exchange_strong(state, st_timeout, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                const auto x = w->x.done;
                if (x) {
                    g.unlock();
                    g.release();
                    ::free(w);
                    if (x == 1) goto done;
                    goto enod;  // x == 2, channel closed
                }
            } else {
                g.unlock();
                g.release();
                goto enod;
            }
        }
    }

enod:
    g_done = false;
    return;
done:
    g_done = true;
}

void pipe_impl::write(void* p, int v) {
    auto sched = gSched;
    _m.lock();
    if (this->is_closed()) {
        _m.unlock();
        goto enod;
    }

    // buffer is neither empty nor full
    if (_rx != _wx) {
        this->_write_block(p, v);
        if (_rx == _wx) _full = 1;
        _m.unlock();
        goto done;
    }

    // buffer is empty
    if (!_full) {
        while (!_wq.empty()) {
            waitx* w = (waitx*)_wq.pop_front();  // wait for read
            decltype(w->state)::value_type state(st_wait);
            if (_ms == (uint32_t)-1 ||
                w->state.compare_exchange_strong(state, st_ready, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                w->x.done = 1;
                if (w->co) {
                    if (w->x.v & 2) _d(w->buf);
                    _c(w->buf, p, v);
                    _m.unlock();
                    w->co->sched->add_ready_task(w->co);
                } else {
                    _d(w->buf);
                    _c(w->buf, p, v);
                    _cv.notify_all();
                    _m.unlock();
                }
                goto done;

            } else { /* timeout */
                ::free(w);
            }
        }

        this->_write_block(p, v);
        if (_rx == _wx) _full = 1;
        _m.unlock();
        goto done;
    }

    // buffer is full
    if (sched) {
        auto co = sched->running();
        waitx* w = this->create_waitx(co, p);
        if (w->buf != p) { /* p is on the coroutine stack */
            _c(w->buf, p, v);
            w->x.v = 1 | 2;
        } else {
            w->x.v = (uint8_t)v;
        }
        _wq.push_back(w);
        _m.unlock();

        co->waitx = (waitx_t*)w;
        if (_ms != (uint32_t)-1) sched->add_timer(_ms);
        sched->yield();

        co->waitx = 0;
        if (!sched->timeout()) {
            ::free(w);
            goto done;
        }
        goto enod;  // timeout

    } else {
        bool r = true;
        waitx* w = this->create_waitx(nullptr, p);
        w->x.v = (uint8_t)v;
        _wq.push_back(w);
        std::unique_lock<std::mutex> g(_m, std::adopt_lock);
        for (;;) {
            if (_ms == (uint32_t)-1) {
                _cv.wait(g);
            } else {
                r = _cv.wait_for(g, std::chrono::milliseconds(_ms)) == std::cv_status::no_timeout;
            }
            decltype(w->state)::value_type state(st_wait);
            if (r || !w->state.compare_exchange_strong(state, st_timeout, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                if (w->x.done) {
                    assert(w->x.done == 1);
                    g.unlock();
                    g.release();
                    ::free(w);
                    goto done;
                }
            } else {
                g.unlock();
                g.release();
                goto enod;
            }
        }
    }

enod:
    g_done = false;
    return;
done:
    g_done = true;
}

void pipe_impl::close() {
    decltype(_closed)::value_type closed{0};
    _closed.compare_exchange_strong(closed, 1, std::memory_order_relaxed,
                                    std::memory_order_relaxed);
    if (closed == 0) {
        std::unique_lock<std::mutex> g(_m);
        if (_rx == _wx && !_full) { /* empty */
            while (!_wq.empty()) {
                waitx* w = (waitx*)_wq.pop_front();  // wait for read
                decltype(w->state)::value_type state(st_wait);
                if (w->state.compare_exchange_strong(state, st_ready, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
                    w->x.done = 2;  // channel closed
                    if (w->co) {
                        w->co->sched->add_ready_task(w->co);
                    } else {
                        _cv.notify_all();
                    }
                } else {
                    ::free(w);
                }
            }
        }
        _closed.store(2, std::memory_order_relaxed);

    } else if (closed == 1) {
        while (_closed.load(std::memory_order_relaxed) != 2) co::sleep(1);
    }
}

pipe::pipe(uint32_t buf_size, uint32_t blk_size, uint32_t ms, pipe::C&& c, pipe::D&& d) {
    // TODO: alignes
    _p = ::malloc(sizeof(pipe_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) pipe_impl(buf_size, blk_size, ms, std::move(c), std::move(d));
}

pipe::pipe(const pipe& p) : _p(p._p) {
    if (_p) god::cast<pipe_impl*>(_p)->ref();
}

pipe::~pipe() {
    const auto p = (pipe_impl*)_p;
    if (p && p->unref() == 0) {
        p->~pipe_impl();
        ::free(_p);
        _p = 0;
    }
}

void pipe::read(void* p) const { god::cast<pipe_impl*>(_p)->read(p); }

void pipe::write(void* p, int v) const { god::cast<pipe_impl*>(_p)->write(p, v); }

bool pipe::done() const { return god::cast<pipe_impl*>(_p)->done(); }

void pipe::close() const { god::cast<pipe_impl*>(_p)->close(); }

bool pipe::is_closed() const { return god::cast<pipe_impl*>(_p)->is_closed(); }

class pool_impl {
  public:
    typedef co::vector<void*> V;

    pool_impl() : _maxcap((size_t)-1), _refn(1) { this->_make_pools(); }

    pool_impl(std::function<void*()>&& ccb, std::function<void(void*)>&& dcb, size_t cap)
        : _maxcap(cap), _refn(1), _ccb(std::move(ccb)), _dcb(std::move(dcb)) {
        this->_make_pools();
    }

    ~pool_impl() {
        this->clear();
        this->_free_pools();
    }

    void* pop();
    void push(void* p);
    void clear();
    size_t size() const;

    void _make_pools() {
        _size = co::sched_num();
        _pools = (V*)::calloc(_size, sizeof(V));
    }

    void _free_pools() {
        for (int i = 0; i < _size; ++i) _pools[i].~V();
        ::free(_pools);
    }

    inline void ref() noexcept { _refn.fetch_add(1, std::memory_order_relaxed); }
    inline uint32_t unref() noexcept { return --_refn; }

  private:
    V* _pools;
    size_t _size;
    size_t _maxcap;
    std::atomic_uint32_t _refn;
    std::function<void*()> _ccb;
    std::function<void(void*)> _dcb;
};

inline void* pool_impl::pop() {
    auto s = gSched;
    CHECK(s) << "must be called in coroutine..";
    auto& v = _pools[s->id()];
    return !v.empty() ? v.pop_back() : (_ccb ? _ccb() : nullptr);
}

inline void pool_impl::push(void* p) {
    if (p) {
        auto s = gSched;
        CHECK(s) << "must be called in coroutine..";
        auto& v = _pools[s->id()];
        (v.size() < _maxcap || !_dcb) ? v.push_back(p) : _dcb(p);
    }
}

// Create n coroutines to clear all the pools, n is number of schedulers.
// clear() blocks untils all the coroutines are done.
void pool_impl::clear() {
    if (xx::is_active()) {
        auto& scheds = co::scheds();
        co::wait_group wg((uint32_t)scheds.size());
        for (auto& s : scheds) {
            s->go([this, wg]() {
                auto& v = this->_pools[gSched->id()];
                if (this->_dcb)
                    for (auto& e : v) this->_dcb(e);
                v.clear();
                wg.done();
            });
        }
        wg.wait();
    } else {
        for (size_t i = 0; i < _size; ++i) {
            auto& v = _pools[i];
            if (this->_dcb)
                for (auto& e : v) this->_dcb(e);
            v.clear();
        }
    }
}

inline size_t pool_impl::size() const {
    auto s = gSched;
    CHECK(s) << "must be called in coroutine..";
    return _pools[s->id()].size();
}

}  // namespace xx

mutex::mutex() {
    // TODO:alignes
    _p = ::malloc(sizeof(xx::mutex_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) xx::mutex_impl();
}

mutex::mutex(const mutex& m) : _p(m._p) {
    if (_p) god::cast<xx::mutex_impl*>(_p)->ref();
}

mutex::~mutex() {
    const auto p = (xx::mutex_impl*)_p;
    if (p && p->unref() == 0) {
        p->~mutex_impl();
        ::free(_p);
        _p = 0;
    }
}

void mutex::lock() const { god::cast<xx::mutex_impl*>(_p)->lock(); }

void mutex::unlock() const { god::cast<xx::mutex_impl*>(_p)->unlock(); }

bool mutex::try_lock() const { return god::cast<xx::mutex_impl*>(_p)->try_lock(); }

event::event(bool manual_reset, bool signaled) {
    // TODO: alignes
    _p = ::malloc(sizeof(xx::event_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) xx::event_impl(manual_reset, signaled);
}

event::event(const event& e) : _p(e._p) {
    if (_p) god::cast<xx::event_impl*>(_p)->ref();
}

event::~event() {
    const auto p = (xx::event_impl*)_p;
    if (p && p->unref() == 0) {
        p->~event_impl();
        ::free(_p);
        _p = 0;
    }
}

bool event::wait(uint32_t ms) const { return god::cast<xx::event_impl*>(_p)->wait(ms); }

void event::signal() const { god::cast<xx::event_impl*>(_p)->signal(); }

void event::reset() const { god::cast<xx::event_impl*>(_p)->reset(); }

sync_event::sync_event(bool manual_reset, bool signaled) {
    // TODO:alignes
    _p = ::malloc(sizeof(xx::sync_event_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) xx::sync_event_impl(manual_reset, signaled);
}

sync_event::~sync_event() {
    if (_p) {
        ((xx::sync_event_impl*)_p)->~sync_event_impl();
        ::free(_p);
        _p = 0;
    }
}

void sync_event::signal() { ((xx::sync_event_impl*)_p)->signal(); }

void sync_event::reset() { ((xx::sync_event_impl*)_p)->reset(); }

void sync_event::wait() { ((xx::sync_event_impl*)_p)->wait(); }

bool sync_event::wait(uint32_t ms) { return ((xx::sync_event_impl*)_p)->wait(ms); }

wait_group::wait_group(uint32_t n) {
    // TODO:: alignes
    _p = ::malloc(sizeof(xx::event_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) xx::event_impl(false, false, n);
}

wait_group::wait_group(const wait_group& wg) : _p(wg._p) {
    if (_p) god::cast<xx::event_impl*>(_p)->ref();
}

wait_group::~wait_group() {
    const auto p = (xx::event_impl*)_p;
    if (p && p->unref() == 0) {
        p->~event_impl();
        ::free(_p);
        _p = 0;
    }
}

void wait_group::add(uint32_t n) const {
    god::cast<xx::event_impl*>(_p)->wg().fetch_add(n /*, std::memory_order_relaxed*/);
}

void wait_group::done() const {
    const auto e = god::cast<xx::event_impl*>(_p);
    const uint32_t x = --e->wg();
    CHECK(x != (uint32_t)-1);
    if (x == 0) e->signal();
}

void wait_group::wait() const { god::cast<xx::event_impl*>(_p)->wait((uint32_t)-1); }

pool::pool() {
    // TODO:: alignes
    _p = ::malloc(sizeof(xx::pool_impl));  //, L1_CACHE_LINE_SIZE);
    new (_p) xx::pool_impl();
}

pool::pool(const pool& p) : _p(p._p) {
    if (_p) god::cast<xx::pool_impl*>(_p)->ref();
}

pool::~pool() {
    const auto p = (xx::pool_impl*)_p;
    if (p && p->unref() == 0) {
        p->~pool_impl();
        ::free(_p);
        _p = 0;
    }
}

pool::pool(std::function<void*()>&& ccb, std::function<void(void*)>&& dcb, size_t cap) {
    _p = ::malloc(sizeof(xx::pool_impl));
    new (_p) xx::pool_impl(std::move(ccb), std::move(dcb), cap);
}

void* pool::pop() const { return god::cast<xx::pool_impl*>(_p)->pop(); }

void pool::push(void* p) const { god::cast<xx::pool_impl*>(_p)->push(p); }

void pool::clear() const { god::cast<xx::pool_impl*>(_p)->clear(); }

size_t pool::size() const { return god::cast<xx::pool_impl*>(_p)->size(); }

}  // namespace co
