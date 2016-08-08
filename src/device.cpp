// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include "device.h"
#include "sync.h"
#include "motion-module.h"
#include "hw-monitor.h"

#include <array>
#include "image.h"

#include <algorithm>
#include <sstream>

using namespace rsimpl;
using namespace rsimpl::motion_module;

const int MAX_FRAME_QUEUE_SIZE = 20;
const int MAX_EVENT_QUEUE_SIZE = 500;
const int MAX_EVENT_TINE_OUT   = 10;

rs_device_base::rs_device_base(std::shared_ptr<rsimpl::uvc::device> device, const rsimpl::static_device_info & info) : device(device), config(info), capturing(false), usb_port_id(""), data_acquisition_active(false), motion_module_ready(false),
    depth(config, RS_STREAM_DEPTH), color(config, RS_STREAM_COLOR), infrared(config, RS_STREAM_INFRARED), infrared2(config, RS_STREAM_INFRARED2), fisheye(config, RS_STREAM_FISHEYE),
    points(depth), rect_color(color), color_to_depth(color, depth), depth_to_color(depth, color), depth_to_rect_color(depth, rect_color), infrared2_to_depth(infrared2,depth), depth_to_infrared2(depth,infrared2),
    max_publish_list_size(MAX_FRAME_QUEUE_SIZE), event_queue_size(MAX_EVENT_QUEUE_SIZE), events_timeout(MAX_EVENT_TINE_OUT)
{
    streams[RS_STREAM_DEPTH    ] = native_streams[RS_STREAM_DEPTH]     = &depth;
    streams[RS_STREAM_COLOR    ] = native_streams[RS_STREAM_COLOR]     = &color;
    streams[RS_STREAM_INFRARED ] = native_streams[RS_STREAM_INFRARED]  = &infrared;
    streams[RS_STREAM_INFRARED2] = native_streams[RS_STREAM_INFRARED2] = &infrared2;
    streams[RS_STREAM_FISHEYE  ] = native_streams[RS_STREAM_FISHEYE]   = &fisheye;
    streams[RS_STREAM_POINTS]                                          = &points;
    streams[RS_STREAM_RECTIFIED_COLOR]                                 = &rect_color;
    streams[RS_STREAM_COLOR_ALIGNED_TO_DEPTH]                          = &color_to_depth;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_COLOR]                          = &depth_to_color;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_RECTIFIED_COLOR]                = &depth_to_rect_color;
    streams[RS_STREAM_INFRARED2_ALIGNED_TO_DEPTH]                      = &infrared2_to_depth;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_INFRARED2]                      = &depth_to_infrared2;
}

rs_device_base::~rs_device_base()
{
    try
    {
        if (capturing) 
            stop(RS_SOURCE_VIDEO);
        if (data_acquisition_active)
            stop(RS_SOURCE_MOTION_TRACKING);
        if (keep_fw_logger_alive)
            stop_fw_logger();
    }
    catch (...) {}
}

bool rs_device_base::supports_option(rs_option option) const 
{ 
    if(uvc::is_pu_control(option)) return true;
    for(auto & o : config.info.options) if(o.option == option) return true;
    return false; 
}

const char* rs_device_base::get_camera_info(rs_camera_info info) const
{ 
    auto it = config.info.camera_info.find(info);
    if (it == config.info.camera_info.end())
        throw std::runtime_error("selected camera info is not supported for this camera!"); 
    return it->second.c_str();
}

void rs_device_base::enable_stream(rs_stream stream, int width, int height, rs_format format, int fps, rs_output_buffer_format output)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(config.info.stream_subdevices[stream] == -1) throw std::runtime_error("unsupported stream");

    config.requests[stream] = { true, width, height, format, fps, output };
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

void rs_device_base::enable_stream_preset(rs_stream stream, rs_preset preset)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(!config.info.presets[stream][preset].enabled) throw std::runtime_error("unsupported stream");

    config.requests[stream] = config.info.presets[stream][preset];
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

rs_motion_intrinsics rs_device_base::get_motion_intrinsics() const
{
    throw std::runtime_error("Motion intrinsics does not supported");
}

rs_extrinsics rs_device_base::get_motion_extrinsics_from(rs_stream from) const
{
    throw std::runtime_error("Motion extrinsics does not supported");
}

void rs_device_base::disable_stream(rs_stream stream)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(config.info.stream_subdevices[stream] == -1) throw std::runtime_error("unsupported stream");

    config.requests[stream] = {};
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

void rs_device_base::set_stream_callback(rs_stream stream, void (*on_frame)(rs_device * device, rs_frame_ref * frame, void * user), void * user)
{
    config.callbacks[stream] = frame_callback_ptr(new frame_callback{ this, on_frame, user });
}

void rs_device_base::set_stream_callback(rs_stream stream, rs_frame_callback* callback)
{
    config.callbacks[stream] = frame_callback_ptr(callback);
}

void rs_device_base::enable_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("motion-tracking cannot be reconfigured after having called rs_start_device()");

    config.data_request.enabled = true;
}

void rs_device_base::disable_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("motion-tracking disabled after having called rs_start_device()");

    config.data_request.enabled = false;
}

void rs_device_base::set_motion_callback(rs_motion_callback* callback)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set motion callback when motion data is active");
    
    // replace previous, if needed
    config.motion_callback = motion_callback_ptr(callback, [](rs_motion_callback* c) { c->release(); });
}

void rs_device_base::start_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("cannot restart data acquisition without stopping first");

    auto parser = std::make_shared<motion_module_parser>();

    // Activate data polling handler
    if (config.data_request.enabled)
    {
        // TODO -replace hard-coded value 3 which stands for fisheye subdevice   
        set_subdevice_data_channel_handler(*device, 3,
            [this, parser](const unsigned char * data, const int size) mutable
        {
            if (motion_module_ready)    //  Flush all received data before MM is fully operational 
            {
                // Parse motion data
                auto events = (*parser)(data, size);

                // Handle events by user-provided handlers
                for (auto & entry : events)
                {
                    // Handle Motion data packets
                    if (config.motion_callback)
                        for (int i = 0; i < entry.imu_entries_num; i++)
                            config.motion_callback->on_event(entry.imu_packets[i]);

                    // Handle Timestamp packets
                    if (config.timestamp_callback)
                    {
                        for (int i = 0; i < entry.non_imu_entries_num; i++)
                        {
                            auto tse = entry.non_imu_packets[i];
                            if (archive)
                                archive->on_timestamp(tse);

                            config.timestamp_callback->on_event(entry.non_imu_packets[i]);
                        }
                    }
                }
            }
        });
    }

    start_data_acquisition(*device);     // activate polling thread in the backend
    data_acquisition_active = true;
}

void rs_device_base::stop_motion_tracking()
{
    if (!data_acquisition_active) throw std::runtime_error("cannot stop data acquisition - is already stopped");
    stop_data_acquisition(*device);
    data_acquisition_active = false;
}

void rs_device_base::set_motion_callback(void(*on_event)(rs_device * device, rs_motion_data data, void * user), void * user)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set motion callback when motion data is active");
    
    // replace previous, if needed
    config.motion_callback = motion_callback_ptr(new motion_events_callback(this, on_event, user), [](rs_motion_callback* c) { delete c; });
}

void rs_device_base::set_timestamp_callback(void(*on_event)(rs_device * device, rs_timestamp_data data, void * user), void * user)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set timestamp callback when motion data is active");

    config.timestamp_callback = timestamp_callback_ptr(new timestamp_events_callback{ this, on_event, user }, [](rs_timestamp_callback* c) { delete c; });
}

void rs_device_base::set_timestamp_callback(rs_timestamp_callback* callback)
{
    // replace previous, if needed
    config.timestamp_callback = timestamp_callback_ptr(callback, [](rs_timestamp_callback* c) { c->release(); });
}

void rs_device_base::start(rs_source source)
{
    if (source & RS_SOURCE_MOTION_TRACKING)
        if (supports(RS_CAPABILITIES_MOTION_EVENTS))
            start_motion_tracking();
        else
             throw std::runtime_error("motion-tracking is not supported by this device");

    if (source & RS_SOURCE_VIDEO)
        start_video_streaming();

}

void rs_device_base::stop(rs_source source)
{
    if (source & RS_SOURCE_VIDEO)
        stop_video_streaming();

    if (source & RS_SOURCE_MOTION_TRACKING)
        if (supports(RS_CAPABILITIES_MOTION_EVENTS))
            stop_motion_tracking();
        else
             throw std::runtime_error("motion-tracking is not supported by this device");
}

std::string hexify(unsigned char n)
{
    std::string res;

    do
    {
        res += "0123456789ABCDEF"[n % 16];
        n >>= 4;
    } while (n);

    reverse(res.begin(), res.end());

    if (res.size() == 1)
    {
        res.insert(0, "0");
    }

    return res;
}

void rs_device_base::start_fw_logger(char fw_log_op_code, int grab_rate_in_ms)
{
    if (keep_fw_logger_alive)
        throw std::logic_error("FW logger already started");

    keep_fw_logger_alive = true;
    fw_logger = std::make_shared<std::thread>([&]() {
        const int data_size = 500;
        hw_monitor::hwmon_cmd cmd((int)fw_log_op_code);
        cmd.Param1 = data_size;
        std::timed_mutex mutex;
        while (keep_fw_logger_alive)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(grab_rate_in_ms));
            hw_monitor::perform_and_send_monitor_command(this->get_device(), mutex, cmd);
            char data[data_size];
            memcpy(data, cmd.receivedCommandData, cmd.receivedCommandDataLength);

            for (int i = 0; i < cmd.receivedCommandDataLength; ++i)
                LOG_DEBUG(hexify(data[i]) << " ");
        }
    });
}

void rs_device_base::stop_fw_logger()
{
    if (!keep_fw_logger_alive)
        throw std::logic_error("FW logger not started");

    keep_fw_logger_alive = false;
    fw_logger->join();
}

void rs_device_base::start_video_streaming()
{
    if(capturing) throw std::runtime_error("cannot restart device without first stopping device");

    auto capture_start_time = std::chrono::high_resolution_clock::now();
    auto selected_modes = config.select_modes();
    auto archive = std::make_shared<syncronizing_archive>(selected_modes, select_key_stream(selected_modes), &max_publish_list_size, &event_queue_size, &events_timeout, capture_start_time);

    for(auto & s : native_streams) s->archive.reset(); // Starting capture invalidates the current stream info, if any exists from previous capture

    // Satisfy stream_requests as necessary for each subdevice, calling set_mode and
    // dispatching the uvc configuration for a requested stream to the hardware
    for(auto mode_selection : selected_modes)
    {
        auto timestamp_reader = create_frame_timestamp_reader(mode_selection.mode.subdevice);
        // Create a stream buffer for each stream served by this subdevice mode
        for(auto & stream_mode : mode_selection.get_outputs())
        {                    
            // If this is one of the streams requested by the user, store the buffer so they can access it
            if(config.requests[stream_mode.first].enabled) native_streams[stream_mode.first]->archive = archive;
        }

        // Copy the callbacks that apply to this stream, so that they can be captured by value
        std::vector<rs_stream> streams;
        for (auto & output : mode_selection.get_outputs())
        {
            streams.push_back(output.first);
        }       
        // Initialize the subdevice and set it to the selected mode
        set_subdevice_mode(*device, mode_selection.mode.subdevice, mode_selection.mode.native_dims.x, mode_selection.mode.native_dims.y, mode_selection.mode.pf.fourcc, mode_selection.mode.fps, 
            [this, mode_selection, archive, timestamp_reader, streams, capture_start_time](const void * frame, std::function<void()> continuation) mutable
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto sys_time = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

            frame_continuation release_and_enqueue(continuation, frame);

            // Ignore any frames which appear corrupted or invalid
            if (!timestamp_reader->validate_frame(mode_selection.mode, frame)) return;

            // Determine the timestamp for this frame
            auto timestamp = timestamp_reader->get_frame_timestamp(mode_selection.mode, frame);
            auto frame_counter = timestamp_reader->get_frame_counter(mode_selection.mode, frame);
            auto recieved_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - capture_start_time).count();
            
            auto requires_processing = mode_selection.requires_processing();

            auto width = mode_selection.get_width();
            auto height = mode_selection.get_height();
            auto fps = mode_selection.get_framerate();
            std::vector<byte *> dest;

            auto stride_x = mode_selection.get_stride_x();
            auto stride_y = mode_selection.get_stride_y();
            for (auto & output : mode_selection.get_outputs())
            {
                LOG_DEBUG("FrameAccepted, RecievedAt," << recieved_time << ", FWTS," << timestamp << ", DLLTS," << recieved_time << ", Type," << rsimpl::get_string(output.first) << ",HasPair,0,F#," << frame_counter);
            }

            for (auto & output : mode_selection.get_outputs())
            {
                auto bpp = get_image_bpp(output.second);
                frame_archive::frame_additional_data additional_data( timestamp,
                    frame_counter,
                    sys_time,
                    width,
                    height,
                    fps,
                    stride_x,
                    stride_y,
                    bpp,
                    output.second,
                    output.first,
                    mode_selection.pad_crop);

                // Obtain buffers for unpacking the frame
                dest.push_back(archive->alloc_frame(output.first, additional_data, requires_processing));

                if (motion_module_ready) // try to correct timestamp only if motion module is enabled
                {
                    archive->correct_timestamp(output.first);
                }
            }
            // Unpack the frame
            if (requires_processing)
            {
                mode_selection.unpack(dest.data(), reinterpret_cast<const byte *>(frame));
            }

            // If any frame callbacks were specified, dispatch them now
            for (size_t i = 0; i < dest.size(); ++i)
            {
                if (!requires_processing)
                {
                    archive->attach_continuation(streams[i], std::move(release_and_enqueue));
                }

                if (config.callbacks[streams[i]])
                {
                    auto frame_ref = archive->track_frame(streams[i]);
                    if (frame_ref)
                    {
                        frame_ref->update_frame_callback_start_ts(std::chrono::high_resolution_clock::now());
                        frame_ref->log_callback_start(capture_start_time);
                        on_before_callback(streams[i], frame_ref, archive);
                        (*config.callbacks[streams[i]])->on_frame(this, frame_ref);
                    }
                }
                else
                {
                    // Commit the frame to the archive
                    archive->commit_frame(streams[i]);
                }
            }
        });
    }
    
    this->archive = archive;
    on_before_start(selected_modes);
    start_streaming(*device, config.info.num_libuvc_transfer_buffers);
    capture_started = std::chrono::high_resolution_clock::now();
    capturing = true;
}

void rs_device_base::stop_video_streaming()
{
    if(!capturing) throw std::runtime_error("cannot stop device without first starting device");
    stop_streaming(*device);
    archive->flush();
    capturing = false;
}

void rs_device_base::wait_all_streams()
{
    if(!capturing) return;
    if(!archive) return;

    archive->wait_for_frames();
}

bool rs_device_base::poll_all_streams()
{
    if(!capturing) return false;
    if(!archive) return false;
    return archive->poll_for_frames();
}

void rs_device_base::release_frame(rs_frame_ref* ref)
{
    archive->release_frame_ref((frame_archive::frame_ref *)ref);
}

rs_frame_ref* ::rs_device_base::clone_frame(rs_frame_ref* frame)
{
    auto result = archive->clone_frame((frame_archive::frame_ref *)frame);
    if (!result) throw std::runtime_error("Not enough resources to clone frame!");
    return result;
}

void rs_device_base::update_device_info(rsimpl::static_device_info& info)
{
    info.options.push_back({ RS_OPTION_FRAMES_QUEUE_SIZE,     1, MAX_FRAME_QUEUE_SIZE,      1, MAX_FRAME_QUEUE_SIZE });
    info.options.push_back({ RS_OPTION_EVENTS_QUEUE_SIZE,     1, MAX_EVENT_QUEUE_SIZE, 1, MAX_EVENT_QUEUE_SIZE });
    info.options.push_back({ RS_OPTION_MAX_TIMESTAMP_LATENCY, 1, MAX_EVENT_TINE_OUT,   1, MAX_EVENT_TINE_OUT });
}

bool rs_device_base::supports(rs_capabilities capability) const
{
    auto found = false;
    auto version_ok = true;
    for (auto supported : config.info.capabilities_vector)
    {
        if (supported.capability == capability)
        {
            found = true;

            firmware_version firmware(get_camera_info(supported.firmware_type));
            if (!firmware.is_between(supported.from, supported.until)) // unsupported due to versioning constraint
            {
                LOG_WARNING("capability " << rs_capabilities_to_string(capability) << " requires " << rs_camera_info_to_string(supported.firmware_type) << " to be from " << supported.from << " up-to " << supported.until << ", but is " << firmware << "!");
                version_ok = false;
            }
        }
    }
    return found && version_ok;
}

void rs_device_base::get_option_range(rs_option option, double & min, double & max, double & step, double & def)
{
    if(uvc::is_pu_control(option))
    {
        int mn, mx, stp, df;
        uvc::get_pu_control_range(get_device(), config.info.stream_subdevices[RS_STREAM_COLOR], option, &mn, &mx, &stp, &df);
        min  = mn;
        max  = mx;
        step = stp;
        def  = df;
        return;
    }

    for(auto & o : config.info.options)
    {
        if(o.option == option)
        {
            min = o.min;
            max = o.max;
            step = o.step;
            def = o.def;
            return;
        }
    }

    throw std::logic_error("range not specified");
}

void rs_device_base::set_options(const rs_option options[], size_t count, const double values[])
{
    for (size_t i = 0; i < count; ++i)
    {
        switch (options[i])
        {
        case  RS_OPTION_FRAMES_QUEUE_SIZE:
            max_publish_list_size = (uint32_t)values[i];
            break;
        case  RS_OPTION_EVENTS_QUEUE_SIZE:
            event_queue_size = (uint32_t)values[i];
            break;
        case  RS_OPTION_MAX_TIMESTAMP_LATENCY:
            events_timeout = (uint32_t)values[i];
            break;
        default:
            LOG_WARNING("Cannot set " << options[i] << " to " << values[i] << " on " << get_name());
            throw std::logic_error("Option unsupported");
            break;
        }
    }
}

void rs_device_base::get_options(const rs_option options[], size_t count, double values[])
{
    for (size_t i = 0; i < count; ++i)
    {
        switch (options[i])
        {
        case  RS_OPTION_FRAMES_QUEUE_SIZE:
            values[i] = max_publish_list_size;
            break;
        case  RS_OPTION_EVENTS_QUEUE_SIZE:
            values[i] = event_queue_size;
            break;
        case  RS_OPTION_MAX_TIMESTAMP_LATENCY:
            values[i] = events_timeout;
            break;
        default:
            LOG_WARNING("Cannot get " << options[i] << " on " << get_name());
            throw std::logic_error("Option unsupported");
            break;
        }
    }
}

void rs_device_base::disable_auto_option(int subdevice, rs_option auto_opt)
{
    static const int reset_state = 0;
    // Probe , then deactivate
    if (uvc::get_pu_control(get_device(), subdevice, auto_opt))
        uvc::set_pu_control(get_device(), subdevice, auto_opt, reset_state);
}

const char * rs_device_base::get_usb_port_id() const
{
    std::lock_guard<std::mutex> lock(usb_port_mutex);
    if (usb_port_id == "") usb_port_id = rsimpl::uvc::get_usb_port_id(*device);
    return usb_port_id.c_str();
}
