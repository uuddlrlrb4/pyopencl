#include "event.h"
#include "command_queue.h"
#include "context.h"
#include "pyhelper.h"

#include <atomic>

namespace pyopencl {

template class clobj<cl_event>;
template void print_arg<cl_event>(std::ostream&, const cl_event&, bool);
template void print_clobj<event>(std::ostream&, const event*);
template void print_buf<cl_event>(std::ostream&, const cl_event*,
                                  size_t, ArgType, bool, bool);

class event_private {
    mutable volatile std::atomic_bool m_finished;
    virtual void
    finish() noexcept
    {}
public:
    virtual
    ~event_private()
    {}
    void
    call_finish() noexcept
    {
        if (m_finished.exchange(true))
            return;
        finish();
    }
    bool
    is_finished() noexcept
    {
        return m_finished;
    }
};

event::event(cl_event event, bool retain, event_private *p)
    : clobj(event), m_p(p)
{
    if (retain) {
        try {
            pyopencl_call_guarded(clRetainEvent, this);
        } catch (...) {
            delete m_p;
            throw;
        }
    }
}

void
event::release_private() noexcept
{
    if (!m_p)
        return;
    if (m_p->is_finished()) {
        delete m_p;
        return;
    }
#if PYOPENCL_CL_VERSION >= 0x1010
    if (support_cb) {
        try {
            event_private *p = m_p;
            set_callback(CL_COMPLETE, [p] (cl_int) {
                p->call_finish();
                delete p;
            });
        } catch (const clerror &e) {
            std::cerr
                << ("PyOpenCL WARNING: a clean-up operation failed "
                    "(dead context maybe?)") << std::endl
                << e.what() << " failed with code " << e.code() << std::endl;
        }
    } else {
#endif
        std::thread t([] (cl_event evt, event_private *p) {
                pyopencl_call_guarded_cleanup(clWaitForEvents, len_arg(evt));
                p->call_finish();
                delete p;
            }, data(), m_p);
        t.detach();
#if PYOPENCL_CL_VERSION >= 0x1010
    }
#endif
}

event::~event()
{
    release_private();
    pyopencl_call_guarded_cleanup(clReleaseEvent, this);
}

generic_info
event::get_info(cl_uint param_name) const
{
    switch ((cl_event_info)param_name) {
    case CL_EVENT_COMMAND_QUEUE:
        return pyopencl_get_opaque_info(command_queue, Event, this, param_name);
    case CL_EVENT_COMMAND_TYPE:
        return pyopencl_get_int_info(cl_command_type, Event,
                                     this, param_name);
    case CL_EVENT_COMMAND_EXECUTION_STATUS:
        return pyopencl_get_int_info(cl_int, Event, this, param_name);
    case CL_EVENT_REFERENCE_COUNT:
        return pyopencl_get_int_info(cl_uint, Event, this, param_name);
#if PYOPENCL_CL_VERSION >= 0x1010
    case CL_EVENT_CONTEXT:
        return pyopencl_get_opaque_info(context, Event, this, param_name);
#endif

    default:
        throw clerror("Event.get_info", CL_INVALID_VALUE);
    }
}

generic_info
event::get_profiling_info(cl_profiling_info param) const
{
    switch (param) {
    case CL_PROFILING_COMMAND_QUEUED:
    case CL_PROFILING_COMMAND_SUBMIT:
    case CL_PROFILING_COMMAND_START:
    case CL_PROFILING_COMMAND_END:
        return pyopencl_get_int_info(cl_ulong, EventProfiling, this, param);
    default:
        throw clerror("Event.get_profiling_info", CL_INVALID_VALUE);
    }
}

void
event::wait() const
{
    pyopencl_call_guarded(clWaitForEvents, len_arg(data()));
    if (m_p) {
        m_p->call_finish();
    }
}

class nanny_event_private : public event_private {
    void *m_ward;
    void finished() noexcept
    {
        void *ward = m_ward;
        m_ward = nullptr;
        py::deref(ward);
    }
    ~nanny_event_private()
    {}
public:
    nanny_event_private(void *ward)
        : m_ward(nullptr)
    {
        m_ward = py::ref(ward);
    }
    PYOPENCL_USE_RESULT PYOPENCL_INLINE void*
    get_ward() const noexcept
    {
        return m_ward;
    }
};

nanny_event::nanny_event(cl_event evt, bool retain, void *ward)
    : event(evt, retain, ward ? new nanny_event_private(ward) : nullptr)
{
}

PYOPENCL_USE_RESULT void*
nanny_event::get_ward() const noexcept
{
    return (get_p() ? static_cast<nanny_event_private*>(get_p())->get_ward() :
            nullptr);
}

}

// c wrapper
// Import all the names in pyopencl namespace for c wrappers.
using namespace pyopencl;

// Event
error*
event__get_profiling_info(clobj_t _evt, cl_profiling_info param,
                          generic_info *out)
{
    auto evt = static_cast<event*>(_evt);
    return c_handle_error([&] {
            *out = evt->get_profiling_info(param);
        });
}

error*
event__wait(clobj_t evt)
{
    return c_handle_error([&] {
            static_cast<event*>(evt)->wait();
        });
}

#if PYOPENCL_CL_VERSION >= 0x1010
error*
event__set_callback(clobj_t _evt, cl_int type, void *pyobj)
{
    auto evt = static_cast<event*>(_evt);
    return c_handle_error([&] {
            pyobj = py::ref(pyobj);
            try {
                evt->set_callback(type, [=] (cl_int status) {
                        py::call(pyobj, status);
                        py::deref(pyobj);
                    });
            } catch (...) {
                py::deref(pyobj);
            }
        });
}
#endif

// Nanny Event
void*
nanny_event__get_ward(clobj_t evt)
{
    return static_cast<nanny_event*>(evt)->get_ward();
}

error*
wait_for_events(const clobj_t *_wait_for, uint32_t num_wait_for)
{
    const auto wait_for = buf_from_class<event>(_wait_for, num_wait_for);
    return c_handle_error([&] {
            pyopencl_call_guarded(clWaitForEvents, wait_for);
        });
}

error*
enqueue_wait_for_events(clobj_t _queue, const clobj_t *_wait_for,
                        uint32_t num_wait_for)
{
    auto queue = static_cast<command_queue*>(_queue);
    const auto wait_for = buf_from_class<event>(_wait_for, num_wait_for);
    return c_handle_error([&] {
            pyopencl_call_guarded(clEnqueueWaitForEvents, queue, wait_for);
        });
}
