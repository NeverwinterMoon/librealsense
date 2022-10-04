// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include <iostream>
#include <map>
#include <unordered_set>
#include <string.h>

#include <librealsense2/utilities/easylogging/easyloggingpp.h>
#include <realdds/dds-device-broadcaster.h>
#include <realdds/dds-device-server.h>
#include <realdds/dds-stream-server.h>
#include <realdds/dds-participant.h>
#include <realdds/topics/notification/notification-msg.h>
#include <realdds/topics/device-info/device-info-msg.h>
#include <fastrtps/types/TypesBase.h>
#include <fastdds/dds/log/Log.hpp>

#include "lrs-device-watcher.h"
#include "lrs-device-controller.h"

#include "tclap/CmdLine.h"
#include "tclap/ValueArg.h"

using namespace TCLAP;
using namespace realdds;

std::vector< std::string > get_supported_streams( rs2::device dev )
{
    auto device_sensors = dev.query_sensors();
    std::unordered_set< std::string > supported_streams_names;
    for( auto sensor : device_sensors )
    {
        auto stream_profiles = sensor.get_stream_profiles();
        std::for_each( stream_profiles.begin(),
                       stream_profiles.end(),
                       [&]( const rs2::stream_profile & sp ) {
                           supported_streams_names.insert( sp.stream_name() );
                       } );
    }

    return std::vector< std::string >( supported_streams_names.begin(), supported_streams_names.end() );
}


rs2::stream_profile get_required_profile( rs2::sensor sensor,
                                          const rs2_stream stream,
                                          const int fps,
                                          rs2_format const format,
                                          const int width,
                                          const int height )
{
    auto sensor_stream_profiles = sensor.get_stream_profiles();
    auto found_profiles = std::find_if( sensor_stream_profiles.begin(),
                                        sensor_stream_profiles.end(),
                                        [&]( rs2::stream_profile sp ) {
                                            auto vp = sp.as< rs2::video_stream_profile >();
                                            return sp.stream_type() == stream && sp.fps() == fps
                                                && sp.format() == format && vp.width() == width
                                                && vp.height() == height;
                                        } );
    if( found_profiles == sensor_stream_profiles.end() )
    {
        throw std::runtime_error( "Could not find required profile" );
    }

    return *found_profiles;
}

void start_streaming( std::shared_ptr< tools::lrs_device_controller > lrs_device_controller,
                      std::shared_ptr< dds_device_server > dds_dev_server,
                      const rs2::stream_profile & stream_profile )
{
    // Configure DDS-server to the required frame header
    realdds::image_header header;
    auto vsp = stream_profile.as< rs2::video_stream_profile >();
    header.format = static_cast< int >( vsp.format() );
    header.height = vsp.height();
    header.width = vsp.width();
    dds_dev_server->set_image_header( stream_profile.stream_name(), header );

    // Start streaming
    lrs_device_controller->start_stream( stream_profile, [&, dds_dev_server]( rs2::frame f ) {
        auto vf = f.as< rs2::video_frame >();
        try
        {
            dds_dev_server->publish_image( vf.get_profile().stream_name(),
                                           (const uint8_t *)f.get_data(),
                                           f.get_data_size() );
        }
        catch( std::exception & e )
        {
            LOG_ERROR( "Exception raised during DDS publish " << vf.get_profile().stream_name()
                                                              << " frame: " << e.what() );
        }
    } );
}

void add_init_device_header_msg( rs2::device dev, std::shared_ptr< dds_device_server > &server )
{   
    topics::device::notification::device_header_msg device_header_msg;
    auto &&sensors = dev.query_sensors();
    size_t num_of_streams = 0;
    for (auto sensor : sensors)
    {
        num_of_streams += sensor.get_stream_profiles().size();
    }
    device_header_msg.num_of_streams = num_of_streams;

    topics::raw::device::notification raw_msg;
    topics::device::notification::construct_raw_message( topics::device::notification::msg_type::DEVICE_HEADER,
                                                         device_header_msg,
                                                         raw_msg );

    server->add_init_msg( std::move( raw_msg ) );
}


void prepare_video_profiles_messeges( rs2::device dev,
                                      std::string sensor_name,
                                      const std::vector< rs2::stream_profile > & stream_profiles,
                                      topics::device::notification::video_stream_profiles_msg & video_stream_profiles_msg )
{
    strcpy_s( video_stream_profiles_msg.group_name, sizeof( video_stream_profiles_msg.group_name ), sensor_name.c_str() );

    int index = 0;
    for( auto & stream_profile : stream_profiles )
    {
        if( stream_profile.is< rs2::video_stream_profile >() )
        {
            const auto & vsp = stream_profile.as< rs2::video_stream_profile >();
            topics::device::notification::video_stream_profile vsp_msg = { static_cast< int8_t >( vsp.stream_index() ),
                                                                           static_cast< int16_t >( vsp.unique_id() ),
                                                                           static_cast< int16_t >( vsp.fps() ),
                                                                           vsp.format(),
                                                                           vsp.stream_type(),
                                                                           static_cast< int16_t >( vsp.width() ),
                                                                           static_cast< int16_t >( vsp.height() ),
                                                                           vsp.is_default() };

            video_stream_profiles_msg.profiles[index++] =  std::move( vsp_msg );
        }
        else
        {
            LOG_ERROR( "got illegal profile with uid:" << stream_profile.unique_id() );
        }
    }

    video_stream_profiles_msg.num_of_profiles = index;
}

void prepare_motion_profiles_messeges( rs2::device dev,
                                       std::string sensor_name,
                                       const std::vector< rs2::stream_profile > & stream_profiles,
                                       topics::device::notification::motion_stream_profiles_msg & motion_stream_profiles_msg )
{
    strcpy_s( motion_stream_profiles_msg.group_name, sizeof( motion_stream_profiles_msg.group_name ), sensor_name.c_str() );

    int index = 0;
    for( auto & stream_profile : stream_profiles )
    {
        if( stream_profile.is< rs2::motion_stream_profile >() )
        {
            const auto & msp = stream_profile.as< rs2::motion_stream_profile >();
            topics::device::notification::motion_stream_profile msp_msg = { static_cast< int8_t >( msp.stream_index() ),
                                                                            static_cast< int16_t >( msp.unique_id() ),
                                                                            static_cast< int16_t >( msp.fps() ),
                                                                            msp.format(),
                                                                            msp.stream_type(),
                                                                            msp.is_default() };
            motion_stream_profiles_msg.profiles[index++] = std::move( msp_msg );
        }
        else
        {
            LOG_ERROR( "got illegal profile with uid:" << stream_profile.unique_id() );
        }
    }

    motion_stream_profiles_msg.num_of_profiles = index;
}

void add_init_profiles_msgs( rs2::device dev, std::shared_ptr<dds_device_server> &server )
{
    auto sensor_idx = 0;

    // For each sensor publish all it's profiles
    for( auto &sensor : dev.query_sensors() )
    {
        auto&& stream_profiles = sensor.get_stream_profiles();
        if( sensor.is< rs2::color_sensor >() || sensor.is< rs2::depth_sensor >() )
        {
            topics::device::notification::video_stream_profiles_msg video_stream_profiles_msg;
            prepare_video_profiles_messeges( dev,
                                             sensor.get_info( RS2_CAMERA_INFO_NAME ),
                                             stream_profiles,
                                             video_stream_profiles_msg );

            // Send video stream profiles
            if (video_stream_profiles_msg.num_of_profiles > 0)
            {
                topics::raw::device::notification raw_video_stream_profiles_msg;
                topics::device::notification::construct_raw_message( topics::device::notification::msg_type::VIDEO_STREAM_PROFILES,
                                                                     video_stream_profiles_msg,
                                                                     raw_video_stream_profiles_msg );

                server->add_init_msg( std::move( raw_video_stream_profiles_msg ) );
            }
        }
        else if( sensor.is< rs2::motion_sensor >() )
        {
            topics::device::notification::motion_stream_profiles_msg motion_stream_profiles_msg;
            prepare_motion_profiles_messeges( dev,
                                              sensor.get_info( RS2_CAMERA_INFO_NAME ),
                                              stream_profiles,
                                              motion_stream_profiles_msg );

            // Send motion stream profiles
            if (motion_stream_profiles_msg.num_of_profiles > 0)
            {
                topics::raw::device::notification raw_motion_stream_profiles_msg;
                topics::device::notification::construct_raw_message( topics::device::notification::msg_type::MOTION_STREAM_PROFILES,
                                                                     motion_stream_profiles_msg,
                                                                     raw_motion_stream_profiles_msg );
                server->add_init_msg( std::move( raw_motion_stream_profiles_msg ) );
            }
        }
        else
            throw std::runtime_error( "Sensor type is not supported (only video & motion sensors are supported)" );

        // Promote to next sensor index
        sensor_idx++;
    }
}


void init_dds_device( rs2::device dev, std::shared_ptr< dds_device_server > & server )
{
    add_init_device_header_msg( dev, server );
    add_init_profiles_msgs( dev, server );
}


std::string get_topic_root( topics::device_info const & dev_info )
{
    // Build device root path (we use a device model only name like DXXX)
    // example: /realsense/D435/11223344
    constexpr char const * DEVICE_NAME_PREFIX = "Intel RealSense ";
    constexpr size_t DEVICE_NAME_PREFIX_CCH = 16;
    // We don't need the prefix in the path
    std::string model_name = dev_info.name;
    if ( model_name.length() > DEVICE_NAME_PREFIX_CCH
       && 0 == strncmp( model_name.data(), DEVICE_NAME_PREFIX, DEVICE_NAME_PREFIX_CCH ) )
    {
        model_name.erase( 0, DEVICE_NAME_PREFIX_CCH );
    }
    constexpr char const * RS_ROOT = "realsense/";
    return RS_ROOT + model_name + '/' + dev_info.serial;
}


topics::device_info rs2_device_to_info( rs2::device const & dev )
{
    topics::device_info dev_info;
    dev_info.name = dev.get_info( RS2_CAMERA_INFO_NAME );
    dev_info.serial = dev.get_info( RS2_CAMERA_INFO_SERIAL_NUMBER );
    dev_info.product_line = dev.get_info( RS2_CAMERA_INFO_PRODUCT_LINE );
    dev_info.locked = ( dev.get_info( RS2_CAMERA_INFO_CAMERA_LOCKED ) == "YES" );

    // Build device topic root path
    dev_info.topic_root = get_topic_root( dev_info );
    return dev_info;
}


struct log_consumer : eprosima::fastdds::dds::LogConsumer
{
    virtual void Consume( const eprosima::fastdds::dds::Log::Entry & e ) override
    {
        using eprosima::fastdds::dds::Log;
        switch( e.kind )
        {
        case Log::Kind::Error:
            LOG_ERROR( "[DDS] " << e.message );
            break;
        case Log::Kind::Warning:
            LOG_WARNING( "[DDS] " << e.message );
            break;
        case Log::Kind::Info:
            LOG_DEBUG( "[DDS] " << e.message );
            break;
        }
    }
};


int main( int argc, char * argv[] )
try
{
    dds_domain_id domain = 0;
    CmdLine cmd( "librealsense rs-dds-server tool, use CTRL + C to stop..", ' ' );
    ValueArg< dds_domain_id > domain_arg( "d",
                                          "domain",
                                          "Select domain ID to listen on",
                                          false,
                                          0,
                                          "0-232" );
    SwitchArg debug_arg( "", "debug", "Enable debug logging", false );

    cmd.add( domain_arg );
    cmd.add( debug_arg );
    cmd.parse( argc, argv );

    // Intercept DDS messages and redirect them to our own logging mechanism
    std::unique_ptr< eprosima::fastdds::dds::LogConsumer > consumer( new log_consumer() );
    eprosima::fastdds::dds::Log::ClearConsumers();
    eprosima::fastdds::dds::Log::RegisterConsumer( std::move( consumer ) );

    if( debug_arg.isSet() )
    {
        rs2::log_to_console( RS2_LOG_SEVERITY_DEBUG );
        eprosima::fastdds::dds::Log::SetVerbosity( eprosima::fastdds::dds::Log::Info );
    }
    else
    {
        rs2::log_to_console( RS2_LOG_SEVERITY_ERROR );
    }
    if( domain_arg.isSet() )
    {
        domain = domain_arg.getValue();
        if( domain > 232 )
        {
            std::cerr << "Invalid domain value, enter a value in the range [0, 232]" << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cout << "Starting RS DDS Server.." << std::endl;

    // Create a DDS publisher
    auto participant = std::make_shared< dds_participant >();
    participant->init( domain, "rs-dds-server" );

    // Run the DDS device broadcaster
    dds_device_broadcaster broadcaster( participant );
    if( !broadcaster.run() )
    {
        std::cerr << "Failure running the DDS Device Broadcaster" << std::endl;
        return EXIT_FAILURE;
    }

    struct device_handler
    {
        topics::device_info info;
        std::shared_ptr< dds_device_server > server;
        std::shared_ptr< tools::lrs_device_controller > controller;
    };
    std::map< rs2::device, device_handler > device_handlers_list;
    
    std::cout << "Start listening to RS devices.." << std::endl;

    // Create a RealSense context
    rs2::context ctx( "{"
        "\"dds-discovery\" : false"
        "}" );

    // Run the LRS device watcher
    tools::lrs_device_watcher dev_watcher( ctx );
    dev_watcher.run(
        // Handle a device connection
        [&]( rs2::device dev ) {

            auto dev_info = rs2_device_to_info( dev );

            // Broadcast the new connected device to all listeners
            broadcaster.add_device( dev_info );

            // Create a supported streams list for initializing the relevant DDS topics
            std::vector<std::string> supported_streams_names_vec = get_supported_streams( dev );

            // Create a dds-device-server for this device
            auto server = std::make_shared< dds_device_server >( participant, dev_info.topic_root );
            // Initialize the DDS device server with the supported streams
            server->init( supported_streams_names_vec );

            // Create a lrs_device_manager for this device
            std::shared_ptr< tools::lrs_device_controller > lrs_device_controller
                = std::make_shared< tools::lrs_device_controller >( dev );

            // Keep a pair of device controller and server per RS device
            device_handlers_list.emplace( dev, device_handler{ dev_info, server, lrs_device_controller } );

            // We add initialization messages to be sent to a new reader (sensors & profiles info).
            init_dds_device( dev, server );

            // Get the desired stream profile
            auto profile = get_required_profile( dev.first< rs2::color_sensor >(),
                                                 RS2_STREAM_COLOR,
                                                 30,
                                                 RS2_FORMAT_RGB8,
                                                 1280,
                                                 720 );

            // Start streaming 
            start_streaming( lrs_device_controller, server, profile );
        },
        // Handle a device disconnection
        [&]( rs2::device dev ) {
            // Remove the dds-server for this device
            auto const & handler = device_handlers_list.at( dev );

            handler.controller->stop_all_streams();
            device_handlers_list.erase( dev );

            // Remove this device from the DDS device broadcaster
            broadcaster.remove_device( handler.info );
        } );

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), 0);// Pend until CTRL + C is pressed 

    std::cout << "Shutting down rs-dds-server..." << std::endl;

    return EXIT_SUCCESS;
}
catch( const rs2::error & e )
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args()
              << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch( const std::exception & e )
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
