// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#pragma once

#include "dds-defines.h"
#include <librealsense2/h/rs_internal.h>

#include <memory>
#include <vector>
#include <functional>
#include <string>

namespace realdds {

namespace topics {
class device_info;
}  // namespace topics


class dds_participant;

// Represents a device via the DDS system. Such a device exists as of its identification by the device-watcher, and
// always contains a device-info and GUID of the remote DataWriter to which it belongs.
// 
// The device may not be ready for use (will not contain sensors, profiles, etc.) until it is "run".
//
class dds_device
{
public:
    class dds_stream
    {
    public:
        dds_stream( rs2_stream type, std::string group_name );

        void add_video_profile( const rs2_video_stream & profile, bool default_profile );
        void add_motion_profile( const rs2_motion_stream & profile, bool default_profile );

        size_t foreach_video_profile( std::function< void( const rs2_video_stream & profile, bool def_prof ) > fn ) const;
        size_t foreach_motion_profile( std::function< void( const rs2_motion_stream & profile, bool def_prof ) > fn ) const;

        //std::string get_name();

    private:
        class impl;
        std::shared_ptr< impl > _impl;
    };

    static std::shared_ptr< dds_device > find( dds_guid const & guid );

    static std::shared_ptr< dds_device > create( std::shared_ptr< dds_participant > const & participant,
                                                 dds_guid const & guid,
                                                 topics::device_info const & info );

    topics::device_info const & device_info() const;

    // The device GUID is that of the DataWriter which declares it!
    dds_guid const & guid() const;

    bool is_running() const;

    // Make the device ready for use. This may take time! Better to do it in the background...
    void run();

    //----------- below this line, a device must be running!

    size_t num_of_streams() const;
    size_t num_of_stream_groups() const;

    //size_t foreach_stream( std::function< void( const std::string & name ) > fn ) const;
    size_t foreach_stream_group( std::function< void( const std::string & name ) > fn ) const;

    size_t foreach_video_profile( std::function< void( const rs2_video_stream & profile, bool def_prof ) > fn ) const;
    size_t foreach_motion_profile( std::function< void( const rs2_motion_stream & profile, bool def_prof ) > fn ) const;

    size_t foreach_video_profile_in_group( const std::string & group_name,
                                           std::function< void( const rs2_video_stream & profile, bool def_prof ) > fn ) const;
    size_t foreach_motion_profile_in_group( const std::string & group_name,
                                            std::function< void( const rs2_motion_stream & profile, bool def_prof ) > fn ) const;

    void open( const std::vector< rs2_video_stream > & streams );
    void close( const std::vector< int16_t >& stream_uids );

private:
    class impl;
    std::shared_ptr< impl > _impl;

    // Ctor is private: use find() or create() instead. Same for dtor -- it should be automatic
    dds_device( std::shared_ptr< impl > );

    //should_lock false only for internal functions already holding the lock to avoid multiple locking
    static std::shared_ptr< dds_device > find_internal( dds_guid const & guid, bool should_lock = true );
};  // class dds_device


}  // namespace realdds
