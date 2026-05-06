#include "gpuRadialPrepGadget.h"
#include "cuNonCartesianSenseOperator.h"
#include "GenericReconJob.h"
#include "cuNDArray_elemwise.h"
#include "cuNDArray_utils.h"
#include "vector_td_operators.h"
#include "GPUTimer.h"
#include "check_CUDA.h"
#include "radial_utilities.h"
#include "hoNDArray_elemwise.h"
#include "hoNDArray_fileio.h"
#include "ismrmrd/xml.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdarg>

namespace Gadgetron{

  gpuRadialPrepGadget::gpuRadialPrepGadget()
    : slices_(-1)
    , sets_(-1)
    , device_number_(-1)
    , mode_(-1)
    , samples_per_profile_(-1)
    , arks_log_fp_(nullptr)
  {
  }
  
  gpuRadialPrepGadget::~gpuRadialPrepGadget() {
    if (arks_log_fp_) {
      fclose(arks_log_fp_);
      arks_log_fp_ = nullptr;
    }
  }

  void gpuRadialPrepGadget::arks_log(const char* fmt, ...) {
    if (!arks_log_fp_) return;
    // timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    fprintf(arks_log_fp_, "%02d-%02d %02d:%02d:%02d.%03ld ",
            tm_buf.tm_mon+1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ts.tv_nsec/1000000);
    va_list args;
    va_start(args, fmt);
    vfprintf(arks_log_fp_, fmt, args);
    va_end(args);
    fflush(arks_log_fp_);
  }
  
  int gpuRadialPrepGadget::process_config(ACE_Message_Block* mb)
  {
    //GDEBUG("gpuRadialPrepGadget::process_config\n");

    // Get configuration values from config file
    //

    mode_ = mode.value();
    device_number_ = deviceno.value();
    rotations_per_reconstruction_ = rotations_per_reconstruction.value();
    buffer_length_in_rotations_ = buffer_length_in_rotations.value();
    buffer_using_solver_ = buffer_using_solver.value();
    output_timing_ = output_timing.value();

    // Currently there are some restrictions on the allowed sliding window configurations
    //
    
    sliding_window_profiles_ = sliding_window_profiles.value();
    sliding_window_rotations_ = sliding_window_rotations.value();

    if( sliding_window_profiles_>0 && sliding_window_rotations_>0 ){
      GDEBUG( "Error: Sliding window reconstruction is not yet supported for both profiles and frames simultaneously.\n" );
      return GADGET_FAIL;
    }

    if( sliding_window_profiles_>0 && rotations_per_reconstruction_>0 ){
      GDEBUG( "Error: Sliding window reconstruction over profiles is not yet supported for multiframe reconstructions.\n" );
      return GADGET_FAIL;
    }
    
    if( sliding_window_rotations_ > 0 && sliding_window_rotations_ >= rotations_per_reconstruction_ ){
      GDEBUG( "Error: Illegal sliding window configuration.\n" );
      return GADGET_FAIL;
    }

    // Setup and validate device configuration
    //

    int number_of_devices;
    if (cudaGetDeviceCount(&number_of_devices)!= cudaSuccess) {
      GDEBUG( "Error: unable to query number of CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (number_of_devices == 0) {
      GDEBUG( "Error: No available CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (device_number_ >= number_of_devices) {
      GDEBUG("Adjusting device number from %d to %d\n", device_number_,  (device_number_%number_of_devices));
      device_number_ = (device_number_%number_of_devices);
    }

    if (cudaSetDevice(device_number_)!= cudaSuccess) {
      GDEBUG( "Error: unable to set CUDA device.\n" );
      return GADGET_FAIL;
    }

    cudaDeviceProp deviceProp;
    if( cudaGetDeviceProperties( &deviceProp, device_number_ ) != cudaSuccess) {
      GDEBUG( "Error: unable to query device properties.\n" );
      return GADGET_FAIL;
    }
    
    unsigned int warp_size = deviceProp.warpSize;

    // Convolution kernel width and oversampling ratio (for the buffer)
    //

    kernel_width_ = buffer_convolution_kernel_width.value();
    oversampling_factor_ = buffer_convolution_oversampling_factor.value();

    // Get the Ismrmrd header
    //
    ISMRMRD::IsmrmrdHeader h;
    ISMRMRD::deserialize(mb->rd_ptr(),h);
    
    
    if (h.encoding.size() != 1) {
      GDEBUG("This Gadget only supports one encoding space\n");
      return GADGET_FAIL;
    }
    
    // Get the encoding space and trajectory description
    ISMRMRD::EncodingSpace e_space = h.encoding[0].encodedSpace;
    ISMRMRD::EncodingSpace r_space = h.encoding[0].reconSpace;
    ISMRMRD::EncodingLimits e_limits = h.encoding[0].encodingLimits;
    ISMRMRD::TrajectoryDescription traj_desc;
    // Matrix sizes (as a multiple of the GPU's warp size)
    //
    
    image_dimensions_.push_back(((e_space.matrixSize.x+warp_size-1)/warp_size)*warp_size);
    image_dimensions_.push_back(((e_space.matrixSize.y+warp_size-1)/warp_size)*warp_size);

    image_dimensions_recon_.push_back(((static_cast<unsigned int>(std::ceil(e_space.matrixSize.x*reconstruction_os_factor_x.value()))+warp_size-1)/warp_size)*warp_size);  
    image_dimensions_recon_.push_back(((static_cast<unsigned int>(std::ceil(e_space.matrixSize.y*reconstruction_os_factor_y.value()))+warp_size-1)/warp_size)*warp_size);
    
    image_dimensions_recon_os_ = uint64d2
      (((static_cast<unsigned int>(std::ceil(image_dimensions_recon_[0]*oversampling_factor_))+warp_size-1)/warp_size)*warp_size,
       ((static_cast<unsigned int>(std::ceil(image_dimensions_recon_[1]*oversampling_factor_))+warp_size-1)/warp_size)*warp_size);
    
    // In case the warp_size constraint kicked in
    oversampling_factor_ = float(image_dimensions_recon_os_[0])/float(image_dimensions_recon_[0]); 
    
    GDEBUG("matrix_size_x : %d, recon: %d, recon_os: %d\n", 
                  image_dimensions_[0], image_dimensions_recon_[0], image_dimensions_recon_os_[0]);

    GDEBUG("matrix_size_y : %d, recon: %d, recon_os: %d\n", 
                  image_dimensions_[1], image_dimensions_recon_[1], image_dimensions_recon_os_[1]);
    
    fov_.push_back(r_space.fieldOfView_mm.x);
    fov_.push_back(r_space.fieldOfView_mm.y);
    fov_.push_back(r_space.fieldOfView_mm.z);

    slices_ = e_limits.slice ? e_limits.slice->maximum + 1 : 1;
    sets_ = e_limits.set ? e_limits.set->maximum + 1 : 1;
    
    // Define some profile counters for book-keeping
    //

    previous_profile_ = boost::shared_array<long>(new long[slices_*sets_]);
    image_counter_ = boost::shared_array<long>(new long[slices_*sets_]);
    profiles_counter_frame_= boost::shared_array<long>(new long[slices_*sets_]);
    profiles_counter_global_= boost::shared_array<long>(new long[slices_*sets_]);
    profiles_per_frame_= boost::shared_array<long>(new long[slices_*sets_]);
    frames_per_rotation_= boost::shared_array<long>(new long[slices_*sets_]);
    buffer_frames_per_rotation_= boost::shared_array<long>(new long[slices_*sets_]);
    buffer_update_needed_ = boost::shared_array<bool>(new bool[slices_*sets_]);
    reconfigure_ = boost::shared_array<bool>(new bool[slices_*sets_]);
    num_coils_ = boost::shared_array<unsigned int>(new unsigned int[slices_*sets_]);
    
    if( !previous_profile_.get() ||
        !image_counter_.get() || 
        !profiles_counter_frame_.get() ||
        !profiles_counter_global_.get() ||
        !profiles_per_frame_.get() || 
        !frames_per_rotation_.get() ||
        !buffer_frames_per_rotation_.get() ||
        !buffer_update_needed_.get() ||
        !num_coils_.get() ||
        !reconfigure_ ){
      GDEBUG("Failed to allocate host memory (1)\n");
      return GADGET_FAIL;
    }

    for( unsigned int i=0; i<slices_*sets_; i++ ){

      previous_profile_[i] = -1;
      image_counter_[i] = 0;
      profiles_counter_frame_[i] = 0;
      profiles_counter_global_[i] = 0;
      profiles_per_frame_[i] = profiles_per_frame.value();
      frames_per_rotation_[i] = frames_per_rotation.value();
      buffer_frames_per_rotation_[i] = buffer_frames_per_rotation.value();
      num_coils_[i] = 0;
      buffer_update_needed_[i] = true;
      reconfigure_[i] = true;

      // Assign some default values ("upper bound estimates") of the (possibly) unknown entities
      //
      
      if( profiles_per_frame_[i] == 0 ){
        profiles_per_frame_[i] = image_dimensions_[0];
      }
      
      if( frames_per_rotation_[i] == 0 ){
        if( mode_ == 2 || mode_ == 3 || mode_ == 4 ) // golden ratio
          frames_per_rotation_[i] = 1;
        else
          frames_per_rotation_[i] = image_dimensions_[0]/profiles_per_frame_[i];
      }
    }
        
    position_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    read_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    phase_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    slice_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);

    if( !position_.get() || !read_dir_.get() || !phase_dir_.get() || !slice_dir_.get() ){
      GDEBUG("Failed to allocate host memory (2)\n");
      return GADGET_FAIL;
    }

    for( unsigned int i=0; i<slices_*sets_; i++ ){
      (position_[i])[0] = (position_[i])[1] = (position_[i])[2] = 0.0f;
      (read_dir_[i])[0] = (read_dir_[i])[1] = (read_dir_[i])[2] = 0.0f;
      (phase_dir_[i])[0] = (phase_dir_[i])[1] = (phase_dir_[i])[2] = 0.0f;
      (slice_dir_[i])[0] = (slice_dir_[i])[1] = (slice_dir_[i])[2] = 0.0f;
    }

    // Allocate accumulation buffer
    //

    allocate_accumulation_buffer( slices_*sets_ );
    
    // Allocate remaining shared_arrays
    //
    
    csm_host_ = boost::shared_array< hoNDArray<float_complext> >(new hoNDArray<float_complext>[slices_*sets_]);
    reg_host_ = boost::shared_array< hoNDArray<float_complext> >(new hoNDArray<float_complext>[slices_*sets_]);

    host_traj_recon_ = boost::shared_array< hoNDArray<floatd2> >(new hoNDArray<floatd2>[slices_*sets_]);
    host_weights_recon_ = boost::shared_array< hoNDArray<float> >(new hoNDArray<float>[slices_*sets_]);

    if( !csm_host_.get() || !reg_host_.get() || !host_traj_recon_.get() || !host_weights_recon_ ){
      GDEBUG("Failed to allocate host memory (3)\n");
      return GADGET_FAIL;
    }

    // FCRL: Initialize custom angle support for mode 4
    // Angles will be read from user_int[fcrl_custom_angle_user_int_index_] of each acquisition header in process()
    fcrl_custom_angle_user_int_index_ = fcrl_custom_angle_user_int_index.value();
    if (fcrl_custom_angle_user_int_index_ < 0 || fcrl_custom_angle_user_int_index_ >= ISMRMRD::ISMRMRD_USER_INTS) {
      GDEBUG("FCRL: Invalid user_int index %d. Must be in range [0, %d). Using default 1.\n",
             fcrl_custom_angle_user_int_index_, ISMRMRD::ISMRMRD_USER_INTS);
      fcrl_custom_angle_user_int_index_ = 1;
    }
    fcrl_use_custom_angles = (mode_ == 4);
    fcrl_total_angles = 0;
    fcrl_custom_angles_deg.clear();
    fcrl_custom_angles_rad.clear();

    if (mode_ == 4) {
      GDEBUG("FCRL: Mode 4 detected. Custom angles will be read from user_int[%d] of each acquisition header.\n",
             fcrl_custom_angle_user_int_index_);
    }

    // ARKS spoke buffer initialization
    arks_buffer_length_TRs_ = buffer_length_TRs.value();
    arks_max_spokes_per_frame_ = max_spokes_per_frame.value();
    arks_enabled_ = (mode_ == 4 && arks_buffer_length_TRs_ > 0);
    arks_spoke_buffer_.clear();

    // ARKS file logging initialization
    arks_log_fp_ = nullptr;
    if (arks_enabled_ && arks_log_enabled.value()) {
      std::string log_path = arks_log_file.value();
      arks_log_fp_ = fopen(log_path.c_str(), "w");
      if (arks_log_fp_) {
        GDEBUG("ARKS: Log file opened: %s\n", log_path.c_str());
        arks_log("ARKS log started. buffer_length_TRs=%ld, max_spokes_per_frame=%ld\n",
                arks_buffer_length_TRs_, arks_max_spokes_per_frame_);
      } else {
        GDEBUG("ARKS: WARNING: Failed to open log file: %s\n", log_path.c_str());
      }
    }

    if (arks_enabled_) {
      GDEBUG("ARKS: Spoke buffer enabled. buffer_length_TRs=%ld, max_spokes_per_frame=%ld\n",
             arks_buffer_length_TRs_, arks_max_spokes_per_frame_);
    }

    return GADGET_OK;
  }

  gpuRadialPrepGadget::ArksGatherResult
  gpuRadialPrepGadget::arks_gather_spokes(long current_tr, unsigned int set, unsigned int slice,
                                           const int32_t* current_user_int)
  {
    ArksGatherResult result;
    result.total_gathered = 0;

    if (!arks_enabled_) return result;

    unsigned int buf_key = set * slices_ + slice;
    auto it = arks_spoke_buffer_.find(buf_key);
    if (it == arks_spoke_buffer_.end() || it->second.empty()) return result;

    const auto &buf = it->second;
    long buf_oldest_tr = buf.front().tr_index;
    long buf_newest_tr = buf.back().tr_index;

    int n_samples = 19;  // user_int[3] = N_samples
    if (n_samples <= 0) return result;
    int half_window = n_samples / 2;  // e.g. 9/2 = 4, so +/- 4 around target

    // Iterate over lag slots: user_int[4] through user_int[7]
    for (int lag_slot = 4; lag_slot <= 7; lag_slot++) {
      int32_t lag = current_user_int[lag_slot];
      if (lag <= 0) continue;  // skip unused lag slots

      long target_tr = current_tr - lag;
      long gather_start = target_tr - half_window;
      long gather_end = target_tr + half_window;

      // Check if target range is within buffer
      if (gather_end < buf_oldest_tr || gather_start > buf_newest_tr) {
        continue;
      }

      // Clamp to buffer bounds
      if (gather_start < buf_oldest_tr) gather_start = buf_oldest_tr;
      if (gather_end > buf_newest_tr) gather_end = buf_newest_tr;

      // Binary search for gather_start in sorted deque (sorted by tr_index)
      // The deque is monotonically increasing by tr_index
      size_t lo = 0, hi = buf.size();
      while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (buf[mid].tr_index < gather_start)
          lo = mid + 1;
        else
          hi = mid;
      }

      // Collect spokes in [gather_start, gather_end]
      int gathered_this_lag = 0;
      for (size_t idx = lo; idx < buf.size() && buf[idx].tr_index <= gather_end; idx++) {
        if (buf[idx].slice == slice && buf[idx].set == set) {
          result.profiles.push_back(buf[idx].data.get());
          result.angles_rad.push_back(buf[idx].angle_rad);
          gathered_this_lag++;
        }
      }

    }

    result.total_gathered = result.profiles.size();
    return result;
  }

  boost::shared_ptr< hoNDArray<floatd2> >
  gpuRadialPrepGadget::arks_build_combined_trajectory_2d(const std::vector<float>& angles_rad)
  {
    long num_profiles = (long)angles_rad.size();
    long total_samples = samples_per_profile_ * num_profiles;

    std::vector<size_t> dims;
    dims.push_back(total_samples);
    dims.push_back(1);  // single frame

    boost::shared_ptr< hoNDArray<floatd2> > host_traj(new hoNDArray<floatd2>(dims));
    float sample_scale = 1.0f / (float)samples_per_profile_;

    for (long p = 0; p < num_profiles; p++) {
      // +PI offset to match the convention used in fcrl_compute_custom_radial_trajectory_2d
      float angle = angles_rad[p] + (float)M_PI;
      float cos_a = std::cos(angle);
      float sin_a = std::sin(angle);
      float bias = samples_per_profile_ * 0.5f;

      for (long s = 0; s < samples_per_profile_; s++) {
        float x = (s - bias) * cos_a * sample_scale;
        float y = (s - bias) * sin_a * sample_scale;
        size_t idx = p * samples_per_profile_ + s;
        (*host_traj)[idx][0] = x;
        (*host_traj)[idx][1] = y;
      }
    }

    return host_traj;
  }

  int gpuRadialPrepGadget::
  process(GadgetContainerMessage<ISMRMRD::AcquisitionHeader> *m1,
          GadgetContainerMessage< hoNDArray< std::complex<float> > > *m2)
  {
    // Noise should have been consumed by the noise adjust (if in the gadget chain)
    //
    
    bool is_noise = m1->getObjectPtr()->isFlagSet(ISMRMRD::ISMRMRD_ACQ_IS_NOISE_MEASUREMENT);
    if (is_noise) { 
      m1->release();
      return GADGET_OK;
    }

    unsigned int profile = m1->getObjectPtr()->idx.kspace_encode_step_1;
    unsigned int slice = m1->getObjectPtr()->idx.slice;
    unsigned int set = m1->getObjectPtr()->idx.set;

    // FCRL: Read custom angle from user_int[fcrl_custom_angle_user_int_index_] for mode 4
    // Raw user_int[index] = angle_in_radians * 10000
    if (mode_ == 4) {
      float angle_rad = (float)m1->getObjectPtr()->user_int[fcrl_custom_angle_user_int_index_] / 10000.0f;
      // Wrap to [0, 2*PI)
      angle_rad = fmod(angle_rad, (float)(2.0 * M_PI));
      if (angle_rad < 0) angle_rad += (float)(2.0 * M_PI);
      float angle_deg = angle_rad * 180.0f / M_PI;
      fcrl_custom_angles_deg.push_back(angle_deg);
      fcrl_custom_angles_rad.push_back(angle_rad);
      fcrl_total_angles = fcrl_custom_angles_rad.size();
      
      if (fcrl_total_angles <= 5 || fcrl_total_angles % 1000 == 0) {
        GDEBUG("FCRL: Acq #%zu user_int[%d] = %d -> %.4f rad (%.2f deg)\n",
               fcrl_total_angles, fcrl_custom_angle_user_int_index_, m1->getObjectPtr()->user_int[fcrl_custom_angle_user_int_index_], angle_rad, angle_deg);
      }
    }

    // ARKS: Populate spoke buffer (mode 4 with buffer_length_TRs > 0)
    if (arks_enabled_) {
      unsigned int buf_key = set * slices_ + slice;
      long current_tr = profiles_counter_global_[buf_key];

      // Build ArksSpoke entry
      ArksSpoke spoke;
      spoke.tr_index = current_tr;
      spoke.angle_rad = fcrl_custom_angles_rad.back();
      memcpy(spoke.user_int, m1->getObjectPtr()->user_int, sizeof(int32_t) * ISMRMRD::ISMRMRD_USER_INTS);
      spoke.slice = slice;
      spoke.set = set;
      spoke.data = std::unique_ptr<ProfileMessage>(duplicate_profile(m2));

      arks_spoke_buffer_[buf_key].push_back(std::move(spoke));

      // Evict spokes older than buffer window
      auto &buf = arks_spoke_buffer_[buf_key];
      while (!buf.empty() && buf.front().tr_index < current_tr - arks_buffer_length_TRs_) {
        buf.pop_front();
      }

      // Periodic logging
      if (current_tr <= 5 || current_tr % 1000 == 0) {
        long oldest_tr = buf.empty() ? -1 : buf.front().tr_index;
        long newest_tr = buf.empty() ? -1 : buf.back().tr_index;
        size_t buf_size = buf.size();
        GDEBUG("ARKS: [set=%u,slice=%u] buffer_size=%zu, TR_range=[%ld,%ld], current_TR=%ld\n",
               set, slice, buf_size, oldest_tr, newest_tr, current_tr);
      }
    }

    static int process_call_count = 0;
    process_call_count++;
    if(process_call_count <= 35 || process_call_count % 32 == 0) {
      GDEBUG("FCRL: process() call #%d, profile=%u, slice=%u, set=%u\n", 
             process_call_count, profile, slice, set);
    }

    // Only when the first profile arrives, do we know the #samples/profile
    //

    if( samples_per_profile_ == -1 )      
      samples_per_profile_ = m1->getObjectPtr()->number_of_samples;
    
    if( samples_per_profile_ != m1->getObjectPtr()->number_of_samples ){
      GDEBUG("Unexpected change in the incoming profiles' lengths\n");
      return GADGET_FAIL;
    }
    
    //GDEBUG("gpuRadialPrepGadget::process\n");

    boost::shared_ptr<GPUTimer> process_timer;
    if( output_timing_ )
      process_timer = boost::shared_ptr<GPUTimer>( new GPUTimer("gpuRadialPrepGadget::process()") );

    // Reconfigure at first pass
    // - or if the number of coil changes
    // - or if the reconfigure_ flag is set

    if( num_coils_[set*slices_+slice] != m1->getObjectPtr()->active_channels ){
      GDEBUG("Reconfiguring due to change in the number of coils\n");
      num_coils_[set*slices_+slice] = m1->getObjectPtr()->active_channels;
      reconfigure(set, slice);
    }

    if( reconfigure_[set*slices_+slice] ){
      GDEBUG("Reconfiguring due to boolean indicator\n");
      reconfigure(set, slice);
    }

    // Get a pointer to the accumulation buffer. 
    //
    
    cuBuffer<float,2> *acc_buffer = get_buffer_ptr(set*slices_+slice);

    // Have the imaging plane changed?
    //

    if( !vec_equal(position_[set*slices_+slice], m1->getObjectPtr()->position) ||
        !vec_equal(read_dir_[set*slices_+slice], m1->getObjectPtr()->read_dir) || 
        !vec_equal(phase_dir_[set*slices_+slice], m1->getObjectPtr()->phase_dir) ||
        !vec_equal(slice_dir_[set*slices_+slice], m1->getObjectPtr()->slice_dir) ){
      
      // Yes indeed, clear the accumulation buffer
      acc_buffer->clear();
      buffer_update_needed_[set*slices_+slice] = true;
      
      memcpy(position_[set*slices_+slice],m1->getObjectPtr()->position,3*sizeof(float));
      memcpy(read_dir_[set*slices_+slice],m1->getObjectPtr()->read_dir,3*sizeof(float));
      memcpy(phase_dir_[set*slices_+slice],m1->getObjectPtr()->phase_dir,3*sizeof(float));
      memcpy(slice_dir_[set*slices_+slice],m1->getObjectPtr()->slice_dir,3*sizeof(float));
    }
        
    bool new_frame_detected = false;

    // Keep track of the incoming profile ids (mode dependent)
    // - to determine the number of profiles per frame
    // - to determine the number of frames per rotation
    //

    if (previous_profile_[set*slices_+slice] >= 0) {

      if ( profile > previous_profile_[set*slices_+slice]) { // this is not the last profile in the frame
        if( mode_ == 0 && frames_per_rotation.value() == 0 ){
          unsigned int acceleration_factor = profile - previous_profile_[set*slices_+slice];
          if( acceleration_factor != frames_per_rotation_[set*slices_+slice] ){
            GDEBUG("Reconfiguring due to change in acceleration factor\n");
            frames_per_rotation_[set*slices_+slice] = acceleration_factor;
            reconfigure(set, slice);
          }
        }
      }
      else{ // This is the first profile in a new frame
        if( profiles_per_frame.value() == 0 && // make sure the user did not specify a desired value for this variable
            profiles_counter_frame_[set*slices_+slice] > 0 &&
            profiles_counter_frame_[set*slices_+slice] != profiles_per_frame_[set*slices_+slice] ){ // a new acceleration factor is detected
          GDEBUG("Reconfiguring due to new slice detection\n");
          new_frame_detected = true;
          profiles_per_frame_[set*slices_+slice] = profiles_counter_frame_[set*slices_+slice];
          if( mode_ == 1 && frames_per_rotation.value() == 0 )
            frames_per_rotation_[set*slices_+slice] = image_dimensions_[0]/profiles_per_frame_[set*slices_+slice];
          reconfigure(set, slice);
        }
      }
    }
    previous_profile_[set*slices_+slice] = profile;

    // Enqueue profile
    // - if 'new_frame_detected' the current profile does not belong to the current frame and we delay enqueuing

    if( !new_frame_detected ) {
      
      // Memory handling is easier if we make copies for our internal queues
      frame_profiles_queue_[set*slices_+slice].push(std::unique_ptr<ProfileMessage>(duplicate_profile(m2)));
      recon_profiles_queue_[set*slices_+slice].push(std::unique_ptr<ProfileMessage>(duplicate_profile(m2)));
    }

    // If the profile is the last of a "true frame" (ignoring any sliding window profiles)
    // - then update the accumulation buffer

    bool is_last_profile_in_frame = (profiles_counter_frame_[set*slices_+slice] == profiles_per_frame_[set*slices_+slice]-1);
    is_last_profile_in_frame |= new_frame_detected;

    GDEBUG("FCRL: profile=%u, counter_frame=%ld, per_frame=%ld, is_last=%d, new_frame=%d\n",
           profile, profiles_counter_frame_[set*slices_+slice], profiles_per_frame_[set*slices_+slice],
           is_last_profile_in_frame, new_frame_detected);

    if( is_last_profile_in_frame ){

      // Extract this frame's samples to update the csm/regularization buffer
      //

      boost::shared_ptr< hoNDArray<float_complext> > host_samples = 
        extract_samples_from_queue( frame_profiles_queue_[set*slices_+slice], false, set, slice );

      if( host_samples.get() == 0x0 ){
        GDEBUG("Failed to extract frame data from queue\n");
        return GADGET_FAIL;
      }
      
      cuNDArray<float_complext> samples( *host_samples );
      
      long profile_offset = profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0);
      boost::shared_ptr< cuNDArray<floatd2> > traj = calculate_trajectory_for_frame(profile_offset, set, slice);

      buffer_update_needed_[set*slices_+slice] |= acc_buffer->add_frame_data( &samples, traj.get() );
    }
    
    // Are we ready to reconstruct (downstream)?
    //
    
    long profiles_per_reconstruction = profiles_per_frame_[set*slices_+slice];
    
    if( rotations_per_reconstruction_ > 0 )
      profiles_per_reconstruction *= (frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_);
    
    bool is_last_profile_in_reconstruction = ( recon_profiles_queue_[set*slices_+slice].size() == profiles_per_reconstruction );

    GDEBUG("FCRL: recon_queue_size=%ld, profiles_per_recon=%ld, is_last_recon=%d, img_queue_size=%ld\n",
           recon_profiles_queue_[set*slices_+slice].size(), profiles_per_reconstruction,
           is_last_profile_in_reconstruction, image_headers_queue_[set*slices_+slice].size());
        
    // Prepare the image header for this frame
    // - if this is indeed the last profile of a new frame
    // - or if we are about to reconstruct due to 'sliding_window_profiles_' > 0

    if( is_last_profile_in_frame || 
        (is_last_profile_in_reconstruction && image_headers_queue_[set*slices_+slice].empty()) ){
      
      GDEBUG("FCRL: Creating image header! Condition: is_last_frame=%d OR (is_last_recon=%d AND queue_empty=%d)\n",
             is_last_profile_in_frame, is_last_profile_in_reconstruction, 
             image_headers_queue_[set*slices_+slice].empty());
      
      GadgetContainerMessage<ISMRMRD::ImageHeader> *header = new GadgetContainerMessage<ISMRMRD::ImageHeader>();
      ISMRMRD::AcquisitionHeader *base_head = m1->getObjectPtr();

      {
        // Initialize header to all zeroes (there is a few fields we do not set yet)
        ISMRMRD::ImageHeader tmp;
        *(header->getObjectPtr()) = tmp;
      }

      header->getObjectPtr()->version = base_head->version;

      header->getObjectPtr()->measurement_uid = base_head->measurement_uid;

      header->getObjectPtr()->matrix_size[0] = image_dimensions_recon_[0];
      header->getObjectPtr()->matrix_size[1] = image_dimensions_recon_[1];
      header->getObjectPtr()->matrix_size[2] = std::max(1L,frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_);

      header->getObjectPtr()->field_of_view[0] = fov_[0];
      header->getObjectPtr()->field_of_view[1] = fov_[1];
      header->getObjectPtr()->field_of_view[2] = fov_[2];

      header->getObjectPtr()->channels = num_coils_[set*slices_+slice];
      header->getObjectPtr()->slice = base_head->idx.slice;
      header->getObjectPtr()->set = base_head->idx.set;

      header->getObjectPtr()->acquisition_time_stamp = base_head->acquisition_time_stamp;
      memcpy(header->getObjectPtr()->physiology_time_stamp, base_head->physiology_time_stamp, sizeof(uint32_t)*ISMRMRD::ISMRMRD_PHYS_STAMPS);

      memcpy(header->getObjectPtr()->position, base_head->position, sizeof(float)*3);
      memcpy(header->getObjectPtr()->read_dir, base_head->read_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->phase_dir, base_head->phase_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->slice_dir, base_head->slice_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->patient_table_position, base_head->patient_table_position, sizeof(float)*3);

      header->getObjectPtr()->data_type = ISMRMRD::ISMRMRD_CXFLOAT;
      header->getObjectPtr()->image_index = image_counter_[set*slices_+slice]++; 
      header->getObjectPtr()->image_series_index = set*slices_+slice;

      image_headers_queue_[set*slices_+slice].push(std::unique_ptr<ImageHeaderMessage>(header));
    }
    
    // If it is time to reconstruct (downstream) then prepare the Sense job
    // 

    if( is_last_profile_in_reconstruction ){
      
      // Update csm and regularization images if the buffer has changed (completed a cycle) 
      // - and at the first pass
      
      if( buffer_update_needed_[set*slices_+slice] || 
          csm_host_[set*slices_+slice].get_number_of_elements() == 0 || 
          reg_host_[set*slices_+slice].get_number_of_elements() == 0 ){

        // Compute and set CSM (in derived Sense/Spirit/... class)
        //

        csm_host_[set*slices_+slice] = *compute_csm( set*slices_+slice );
	                
        // Compute regularization image
        //
        
        reg_host_[set*slices_+slice] = *compute_reg( set, slice, new_frame_detected );

        buffer_update_needed_[set*slices_+slice] = false;
      }

      // Prepare data array of the profiles for the downstream reconstruction
      //
      
      boost::shared_ptr< hoNDArray<float_complext> > samples_host = 
        extract_samples_from_queue( recon_profiles_queue_[set*slices_+slice], true, set, slice );
      
      if( samples_host.get() == nullptr ){
        GDEBUG("Failed to extract frame data from queue\n");
        return GADGET_FAIL;
      }

      // ARKS Stage 3: Gather historical spokes and merge with current frame
      bool arks_merged = false;
      if (arks_enabled_) {
        long profile_offset_arks = profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0);
        ArksGatherResult gathered = arks_gather_spokes(profile_offset_arks, set, slice, m1->getObjectPtr()->user_int);

        if (gathered.total_gathered > 0) {
          long current_profiles = (long)recon_profiles_queue_[set*slices_+slice].size();
          // recon queue was just emptied, so current_profiles=0; use profiles_per_reconstruction instead
          current_profiles = profiles_per_frame_[set*slices_+slice];
          if (rotations_per_reconstruction_ > 0)
            current_profiles *= (frames_per_rotation_[set*slices_+slice] * rotations_per_reconstruction_);
          long total_profiles = current_profiles + (long)gathered.total_gathered;
          unsigned int ncoils = num_coils_[set*slices_+slice];

          // Collect current window angles
          std::vector<float> combined_angles;
          long first_profile_idx = std::max(0L, profile_offset_arks - current_profiles + 1);
          for (long i = 0; i < current_profiles; i++) {
            combined_angles.push_back(fcrl_get_custom_angle(first_profile_idx + i));
          }
          // Append gathered historical angles
          combined_angles.insert(combined_angles.end(),
                                 gathered.angles_rad.begin(), gathered.angles_rad.end());

          // Build combined data array: [total_profiles * spp, ncoils]
          std::vector<size_t> combined_dims = {
            (size_t)(total_profiles * samples_per_profile_), (size_t)ncoils
          };
          boost::shared_ptr< hoNDArray<float_complext> > combined(new hoNDArray<float_complext>(combined_dims));
          memset(combined->get_data_ptr(), 0, combined->get_number_of_bytes());

          // Copy current window data (layout: coil-major, each coil block = spp * current_profiles)
          for (unsigned int c = 0; c < ncoils; c++) {
            float_complext* src = samples_host->get_data_ptr() + c * samples_per_profile_ * current_profiles;
            float_complext* dst = combined->get_data_ptr() + c * samples_per_profile_ * total_profiles;
            memcpy(dst, src, samples_per_profile_ * current_profiles * sizeof(float_complext));
          }

          // Copy gathered historical spoke data
          for (size_t g = 0; g < gathered.profiles.size(); g++) {
            hoNDArray< std::complex<float> >* pdata = gathered.profiles[g]->getObjectPtr();
            for (unsigned int c = 0; c < ncoils; c++) {
              float_complext* dst = combined->get_data_ptr()
                + c * samples_per_profile_ * total_profiles
                + (current_profiles + (long)g) * samples_per_profile_;
              std::complex<float>* src = pdata->get_data_ptr() + c * pdata->get_size(0);
              memcpy(dst, src, samples_per_profile_ * sizeof(float_complext));
            }
          }

          samples_host = combined;

          // Build combined trajectory from explicit angle list
          boost::shared_ptr< hoNDArray<floatd2> > combined_traj = arks_build_combined_trajectory_2d(combined_angles);
          host_traj_recon_[set*slices_+slice] = *combined_traj;

          // Compute DCW using golden ratio analytical formula (stable for non-uniform angular distributions)
          host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_golden_ratio_2d<float>(
            samples_per_profile_, total_profiles, oversampling_factor_,
            1.0f / (float(samples_per_profile_) / float(image_dimensions_recon_[0])),
            0, GR_SMALLEST)->to_host();

          arks_merged = true;

          arks_log("RECON [set=%u,slice=%u] TR=%ld current=%ld gathered=%zu total=%ld\n",
                   set, slice, profile_offset_arks, current_profiles, gathered.total_gathered, total_profiles);
        }
      }

      // Normal trajectory/DCW path (non-ARKS or no gathered spokes)
      if (!arks_merged) {
        if( mode_ == 2 || mode_ == 3 || mode_ == 4 || rotations_per_reconstruction_ == 0 ){
          calculate_trajectory_for_reconstruction
            ( profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0), set, slice );
        }
        // Always recompute DCW to match trajectory dimensions
        // (ARKS frames may have changed host_weights_recon_ to a different size)
        calculate_density_compensation_for_reconstruction(set, slice);
      }
      
      // Set up Sense job
      //

      GadgetContainerMessage< GenericReconJob >* m4 = new GadgetContainerMessage< GenericReconJob >();
	
      m4->getObjectPtr()->dat_host_ = samples_host;
      m4->getObjectPtr()->tra_host_ = boost::shared_ptr< hoNDArray<floatd2> >(new hoNDArray<floatd2>(host_traj_recon_[set*slices_+slice]));
      m4->getObjectPtr()->dcw_host_ = boost::shared_ptr< hoNDArray<float> >(new hoNDArray<float>(host_weights_recon_[set*slices_+slice]));
      m4->getObjectPtr()->csm_host_ = boost::shared_ptr< hoNDArray<float_complext> >( new hoNDArray<float_complext>(csm_host_[set*slices_+slice]));
      m4->getObjectPtr()->reg_host_ = boost::shared_ptr< hoNDArray<float_complext> >( new hoNDArray<float_complext>(reg_host_[set*slices_+slice]));

      // Pull the image headers out of the queue
      //

      long frames_per_reconstruction = 
        std::max( 1L, frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_ );
      
      if( image_headers_queue_[set*slices_+slice].size() != frames_per_reconstruction ){
        m4->release();
        GDEBUG("Unexpected size of image header queue: %d, %d\n", 
                      image_headers_queue_[set*slices_+slice].size(), frames_per_reconstruction);
        return GADGET_FAIL;
      }

      m4->getObjectPtr()->image_headers_ =
        boost::shared_array<ISMRMRD::ImageHeader>( new ISMRMRD::ImageHeader[frames_per_reconstruction] );
      
      for( unsigned int i=0; i<frames_per_reconstruction; i++ ){	

        ImageHeaderMessage *mbq = image_headers_queue_[set*slices_+slice].front().release();
        image_headers_queue_[set*slices_+slice].pop();

        GadgetContainerMessage<ISMRMRD::ImageHeader> *m = AsContainerMessage<ISMRMRD::ImageHeader>(mbq);
        m4->getObjectPtr()->image_headers_[i] = *m->getObjectPtr();

        // In sliding window mode the header might need to go back at the end of the queue for reuse
        // 
	
        if( i >= frames_per_reconstruction-sliding_window_rotations_*frames_per_rotation_[set*slices_+slice] ){
          image_headers_queue_[set*slices_+slice].push(std::unique_ptr<ImageHeaderMessage>(m));
        }
        else {
          m->release();
        }
      }      
      
      // The Sense Job needs an image header as well. 
      // Let us just copy the initial one...

      GadgetContainerMessage<ISMRMRD::ImageHeader> *m3 = new GadgetContainerMessage<ISMRMRD::ImageHeader>;
      *m3->getObjectPtr() = m4->getObjectPtr()->image_headers_[0];
      m3->cont(m4);
      
      //GDEBUG("Putting job on queue\n");
      
      if (this->next()->putq(m3) < 0) {
        GDEBUG("Failed to put job on queue.\n");
        m3->release();
        return GADGET_FAIL;
      }
    }
    
    if( is_last_profile_in_frame )
      profiles_counter_frame_[set*slices_+slice] = 0;
    else{
      profiles_counter_frame_[set*slices_+slice]++;
    }

    if( new_frame_detected ){

      // This is the first profile of the next frame, enqueue.
      // We have encountered deadlocks if the same profile is enqueued twice in different queues. Hence the copy.
      
      frame_profiles_queue_[set*slices_+slice].push(std::unique_ptr<ProfileMessage>(duplicate_profile(m2)));
      recon_profiles_queue_[set*slices_+slice].push(std::unique_ptr<ProfileMessage>(duplicate_profile(m2)));

      profiles_counter_frame_[set*slices_+slice]++;
    }

    profiles_counter_global_[set*slices_+slice]++;

    if( output_timing_ )
      process_timer.reset();
    
    m1->release(); // the internal queues hold copies
    return GADGET_OK;
  }
  
  int 
  gpuRadialPrepGadget::calculate_trajectory_for_reconstruction(long profile_offset, unsigned int set, unsigned int slice)
  {   
    //GDEBUG("Calculating trajectory for reconstruction\n");

    switch(mode_){
      
    case 0:
    case 1:
      {
        if( rotations_per_reconstruction_ == 0 ){

          long local_frame = (profile_offset/profiles_per_frame_[set*slices_+slice])%frames_per_rotation_[set*slices_+slice];
          float angular_offset = M_PI/float(profiles_per_frame_[set*slices_+slice])*float(local_frame)/float(frames_per_rotation_[set*slices_+slice]);	  

          host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_fixed_angle_2d<float>
            ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, angular_offset )->to_host();	
        }
        else{
          host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_fixed_angle_2d<float>
            ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], frames_per_rotation_[set*slices_+slice] )->to_host();
        }
      }
      break;
      
    case 2:
    case 3:
      {
        if( rotations_per_reconstruction_ == 0 ){	  
          unsigned int first_profile_in_reconstruction = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
          host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
            ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_reconstruction,
              (mode_==2) ? GR_ORIGINAL : GR_SMALLEST )->to_host();	
        }
        else{
          unsigned int first_profile_in_reconstruction = 
            std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_+1);
          host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
            ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 
              frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_, first_profile_in_reconstruction,
              (mode_==2) ? GR_ORIGINAL : GR_SMALLEST )->to_host();
        }	  
      }
      break;

    case 4:
      {
        if (fcrl_use_custom_angles) {
          // FCRL: Use custom angles from CSV
          if( rotations_per_reconstruction_ == 0 ){	  
            unsigned int first_profile_in_reconstruction = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
            host_traj_recon_[set*slices_+slice] = *fcrl_compute_custom_radial_trajectory_2d(
              samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_reconstruction)->to_host();	
          }
          else{
            unsigned int first_profile_in_reconstruction = 
              std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_+1);
            host_traj_recon_[set*slices_+slice] = *fcrl_compute_custom_radial_trajectory_2d(
              samples_per_profile_, 
              profiles_per_frame_[set*slices_+slice],
              frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_, 
              first_profile_in_reconstruction)->to_host();
          }
        } else {
          // Fallback to golden angle (same as mode 3)
          if( rotations_per_reconstruction_ == 0 ){	  
            unsigned int first_profile_in_reconstruction = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
            host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
              ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_reconstruction, GR_SMALLEST )->to_host();	
          }
          else{
            unsigned int first_profile_in_reconstruction = 
              std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_+1);
            host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
              ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 
                frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_, first_profile_in_reconstruction, GR_SMALLEST )->to_host();
          }
        }
      }
      break;
	
    default:
      GDEBUG("Illegal trajectory mode\n");
      return GADGET_FAIL;
      break;
    }
    return GADGET_OK;
  }  

  int
  gpuRadialPrepGadget::calculate_density_compensation_for_reconstruction( unsigned int set, unsigned int slice)
  {
    //GDEBUG("Calculating dcw for reconstruction\n");
    
    switch(mode_){
      
    case 0:
    case 1:
      host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_fixed_angle_2d<float>
        ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 
          1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) )->to_host();
      break;
      
    case 2:
    case 3:
    case 4:
      host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_golden_ratio_2d<float>
        ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 
          1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])),0,
          (mode_==2) ? GR_ORIGINAL : GR_SMALLEST )->to_host();
      break;
      
    default:
      GDEBUG("Illegal dcw mode\n");
      return GADGET_FAIL;
      break;
    }
    return GADGET_OK;
  }
  
  boost::shared_ptr< cuNDArray<floatd2> > 
  gpuRadialPrepGadget::calculate_trajectory_for_frame(long profile_offset, unsigned int set, unsigned int slice)
  {
    //GDEBUG("Calculating trajectory for buffer frame\n");

    boost::shared_ptr< cuNDArray<floatd2> > result;

    switch(mode_){

    case 0:
    case 1:
      {
        long local_frame = (profile_offset/profiles_per_frame_[set*slices_+slice])%frames_per_rotation_[set*slices_+slice];
        float angular_offset = M_PI/float(profiles_per_frame_[set*slices_+slice])*float(local_frame)/float(frames_per_rotation_[set*slices_+slice]);	  

        result = compute_radial_trajectory_fixed_angle_2d<float>
          ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, angular_offset );  
      }
      break;
	
    case 2:
    case 3:
      { 
        unsigned int first_profile_in_buffer = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
        result = compute_radial_trajectory_golden_ratio_2d<float>
          ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_buffer,
            (mode_==2) ? GR_ORIGINAL : GR_SMALLEST );
      }
      break;

    case 4:
      {
        unsigned int first_profile_in_buffer = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
        if (fcrl_use_custom_angles) {
          // FCRL: Use custom angles from CSV
          result = fcrl_compute_custom_radial_trajectory_2d(
            samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_buffer);
        } else {
          // Fallback to golden angle (same as mode 3)
          result = compute_radial_trajectory_golden_ratio_2d<float>
            ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_buffer, GR_SMALLEST );
        }
      }
      break;	
	
    default:
      GDEBUG("Illegal trajectory mode\n");
      break;
    }
    
    return result;
  }

  boost::shared_ptr< cuNDArray<float> >
  gpuRadialPrepGadget::calculate_density_compensation_for_frame(unsigned int set, unsigned int slice)
  {    
    //GDEBUG("Calculating dcw for buffer frame\n");

    switch(mode_){
      
    case 0:
    case 1:
      return compute_radial_dcw_fixed_angle_2d<float>
        ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      break;
      
    case 2:
    case 3:
    case 4:
      return compute_radial_dcw_golden_ratio_2d<float>
        ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 
          1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])),0,
          (mode_==2) ? GR_ORIGINAL : GR_SMALLEST );
      break;
      
    default:
      GDEBUG("Illegal dcw mode\n");
      return boost::shared_ptr< cuNDArray<float> >();
      break;
    }   
  }


  boost::shared_ptr< cuNDArray<floatd2> > 
  gpuRadialPrepGadget::calculate_trajectory_for_rhs(long profile_offset, unsigned int set, unsigned int slice)
  {
    //GDEBUG("Calculating trajectory for rhs\n");

    switch(mode_){

    case 0:
    case 1:
      return compute_radial_trajectory_fixed_angle_2d<float>
        ( samples_per_profile_, profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice], 1 );
      break;
	
    case 2:
    case 3:
      { 
        unsigned int first_profile = 
          std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*
                   buffer_frames_per_rotation_[set*slices_+slice]*
                   buffer_length_in_rotations_+1);

        return compute_radial_trajectory_golden_ratio_2d<float>
          ( samples_per_profile_, 
            profiles_per_frame_[set*slices_+slice]*
            buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_, 
            1, first_profile,
            (mode_==2) ? GR_ORIGINAL : GR_SMALLEST );
      }
      break;

    case 4:
      {
        unsigned int first_profile = 
          std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*
                   buffer_frames_per_rotation_[set*slices_+slice]*
                   buffer_length_in_rotations_+1);

        if (fcrl_use_custom_angles) {
          // FCRL: Use custom angles from CSV
          return fcrl_compute_custom_radial_trajectory_2d(
            samples_per_profile_, 
            profiles_per_frame_[set*slices_+slice]*
            buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_, 
            1, first_profile);
        } else {
          // Fallback to golden angle (same as mode 3)
          return compute_radial_trajectory_golden_ratio_2d<float>
            ( samples_per_profile_, 
              profiles_per_frame_[set*slices_+slice]*
              buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_, 
              1, first_profile, GR_SMALLEST );
        }
      }
      break;	
	
    default:
      GDEBUG("Illegal trajectory mode\n");
      return boost::shared_ptr< cuNDArray<floatd2> >();
      break;
    }
  }
  
  boost::shared_ptr< cuNDArray<float> >
  gpuRadialPrepGadget::calculate_density_compensation_for_rhs(unsigned int set, unsigned int slice)
  {
    //GDEBUG("Calculating dcw for rhs\n");
    
    switch(mode_){
      
    case 0:
    case 1:
      {
        unsigned int num_profiles = 
          profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice];

        return compute_radial_dcw_fixed_angle_2d<float>
          ( samples_per_profile_, num_profiles, oversampling_factor_, 
            1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      }
      break;
      
    case 2:
    case 3:
    case 4:
      {
        unsigned int num_profiles = 
          profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_;

        return compute_radial_dcw_golden_ratio_2d<float>
          ( samples_per_profile_, num_profiles, oversampling_factor_, 
            1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])),0,
            (mode_==2) ? GR_ORIGINAL : GR_SMALLEST );
      }
      break;
      
    default:
      GDEBUG("Illegal dcw mode\n");
      return boost::shared_ptr< cuNDArray<float> >();
      break;
    }
  }

  boost::shared_ptr< hoNDArray<float_complext> > gpuRadialPrepGadget::
  extract_samples_from_queue( std::queue<std::unique_ptr<ProfileMessage>> &queue, bool sliding_window,
                              unsigned int set, unsigned int slice )
  {    
    //GDEBUG("Emptying queue...\n");

    unsigned int profiles_buffered = queue.size();
    
    std::vector<size_t> dims;
    dims.push_back(samples_per_profile_*profiles_buffered);
    dims.push_back(num_coils_[set*slices_+slice]);
    
    boost::shared_ptr< hoNDArray<float_complext> > host_samples(new hoNDArray<float_complext>(dims));
    
    for (unsigned int p=0; p<profiles_buffered; p++) {
      ProfileMessage *mbq = queue.front().release();
      queue.pop();

      GadgetContainerMessage< hoNDArray< std::complex<float> > > *daq = AsContainerMessage<hoNDArray< std::complex<float> > >(mbq);
	
      if (!daq) {
        GDEBUG("Unable to interpret data on message queue\n");
        return boost::shared_ptr< hoNDArray<float_complext> >();
      }
	
      for (unsigned int c = 0; c < num_coils_[set*slices_+slice]; c++) {
	
        float_complext *data_ptr = host_samples->get_data_ptr();
        data_ptr += c*samples_per_profile_*profiles_buffered+p*samples_per_profile_;
	    
        std::complex<float> *r_ptr = daq->getObjectPtr()->get_data_ptr();
        r_ptr += c*daq->getObjectPtr()->get_size(0);
	  
        memcpy(data_ptr,r_ptr,samples_per_profile_*sizeof(float_complext));
      }

      // In sliding window mode the profile might need to go back at the end of the queue
      // 
      
      long profiles_in_sliding_window = sliding_window_profiles_ + 
        profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*sliding_window_rotations_;

      if( sliding_window && p >= (profiles_buffered-profiles_in_sliding_window) )
        queue.push(std::unique_ptr<ProfileMessage>(mbq));
      else
        mbq->release();
    } 
    
    return host_samples;
  }
  
  GadgetContainerMessage< hoNDArray< std::complex<float> > >*
  gpuRadialPrepGadget::duplicate_profile( GadgetContainerMessage< hoNDArray< std::complex<float> > > *profile )
  {
    GadgetContainerMessage< hoNDArray< std::complex<float> > > *copy = 
      new GadgetContainerMessage< hoNDArray< std::complex<float> > >();
    
    *copy->getObjectPtr() = *profile->getObjectPtr();
    
    return copy;
  }

  void gpuRadialPrepGadget::reconfigure(unsigned int set, unsigned int slice, bool use_dcw)
  {    
    GDEBUG("\nReconfiguring:\n#profiles/frame:%d\n#frames/rotation: %d\n#rotations/reconstruction:%d\n", 
                  profiles_per_frame_[set*slices_+slice], frames_per_rotation_[set*slices_+slice], rotations_per_reconstruction_);

    calculate_trajectory_for_reconstruction(0, set, slice);
    calculate_density_compensation_for_reconstruction(set, slice);
    
    buffer_frames_per_rotation_[set*slices_+slice] = buffer_frames_per_rotation.value();

    if( buffer_frames_per_rotation_[set*slices_+slice] == 0 ){
      if( mode_ == 2 || mode_ == 3 || mode_ == 4 )
        buffer_frames_per_rotation_[set*slices_+slice] = 
          image_dimensions_recon_os_[0]/profiles_per_frame_[set*slices_+slice];
      else
        buffer_frames_per_rotation_[set*slices_+slice] = frames_per_rotation_[set*slices_+slice];
    }
    
    cuBuffer<float,2> *acc_buffer = get_buffer_ptr(set*slices_+slice);

    acc_buffer->setup( from_std_vector<size_t,2>(image_dimensions_recon_), image_dimensions_recon_os_, 
                       kernel_width_, num_coils_[set*slices_+slice], 
                       buffer_length_in_rotations_, buffer_frames_per_rotation_[set*slices_+slice] );
    
    if(use_dcw){
      boost::shared_ptr< cuNDArray<float> > device_weights_frame = calculate_density_compensation_for_frame(set, slice);
      acc_buffer->set_dcw(device_weights_frame);
    }

    reconfigure_[set*slices_+slice] = false;
  }

  // FCRL: Get custom angle for given acquisition index
  float gpuRadialPrepGadget::fcrl_get_custom_angle(long acq_index)
  {
    if (fcrl_use_custom_angles && acq_index >= 0 && acq_index < (long)fcrl_total_angles) {
      return fcrl_custom_angles_rad[acq_index];
    }
    // Fallback to GR_SMALLEST golden angle if index out of range
    const float small_golden_angle = M_PI * (3.0f - std::sqrt(5.0f)) * 0.5f; // ~1.19998 radians (~68.75 degrees), matches GR_SMALLEST
    return acq_index * small_golden_angle;
  }

  // FCRL: Compute custom angle radial trajectory for mode 4
  boost::shared_ptr< cuNDArray<floatd2> > gpuRadialPrepGadget::fcrl_compute_custom_radial_trajectory_2d(
    long num_samples_per_profile, long num_profiles_per_frame, long num_frames, long first_profile_index)
  {
    GDEBUG("FCRL: Computing custom trajectory: samples=%ld, profiles_per_frame=%ld, num_frames=%ld, first_idx=%ld\n", 
           num_samples_per_profile, num_profiles_per_frame, num_frames, first_profile_index);
    
    // Match the format of compute_radial_trajectory_golden_ratio_2d:
    // dims = [samples_per_profile * profiles_per_frame, num_frames]
    long samples_per_frame = num_samples_per_profile * num_profiles_per_frame;
    
    std::vector<size_t> dims;
    dims.push_back(samples_per_frame);
    dims.push_back(num_frames);
    
    boost::shared_ptr< hoNDArray<floatd2> > host_traj(new hoNDArray<floatd2>(dims));
    if (!host_traj.get()) {
      GDEBUG("FCRL: ERROR - Failed to allocate host trajectory array\n");
      return boost::shared_ptr< cuNDArray<floatd2> >();
    }
    
    float sample_scale = 1.0f / (float)num_samples_per_profile;
    
    for (long frame = 0; frame < num_frames; frame++) {
      for (long profile = 0; profile < num_profiles_per_frame; profile++) {
        // Global profile index: first_profile + frame * profiles_per_frame + profile_within_frame
        long global_profile_idx = first_profile_index + frame * num_profiles_per_frame + profile;
        float angle = fcrl_get_custom_angle(global_profile_idx);
        // Add PI offset to match golden ratio kernel convention:
        // kernel uses: gad_sincos( (profile+offset)*angle_step + PI, ... )
        angle += (float)M_PI;
        
        if (frame < 2 && profile < 2) { // Debug first few angles
          GDEBUG("FCRL: Frame %ld Profile %ld (global %ld) angle = %.4f rad (%.2f deg) [after +PI]\n", 
                 frame, profile, global_profile_idx, angle, angle * 180.0f / M_PI);
        }
        
        float cos_angle = std::cos(angle);
        float sin_angle = std::sin(angle);
        
        for (long sample = 0; sample < num_samples_per_profile; sample++) {
          // Match the coordinate computation from golden ratio kernel:
          // sample_pos = (sample_idx - bias) * cos/sin(angle) / samples_per_profile
          float bias = num_samples_per_profile * 0.5f;
          float sample_pos_x = (sample - bias) * cos_angle * sample_scale;
          float sample_pos_y = (sample - bias) * sin_angle * sample_scale;
          
          // Linear index: frame * samples_per_frame + profile * samples_per_profile + sample
          size_t idx = frame * samples_per_frame + profile * num_samples_per_profile + sample;
          (*host_traj)[idx][0] = sample_pos_x;
          (*host_traj)[idx][1] = sample_pos_y;
        }
      }
    }
    
    GDEBUG("FCRL: Creating cuNDArray from host trajectory, total elements=%ld\n", samples_per_frame * num_frames);
    boost::shared_ptr< cuNDArray<floatd2> > result(new cuNDArray<floatd2>(*host_traj));
    if (!result.get()) {
      GDEBUG("FCRL: ERROR - Failed to create cuNDArray\n");
      return boost::shared_ptr< cuNDArray<floatd2> >();
    }
    
    GDEBUG("FCRL: Custom trajectory successfully created\n");
    return result;
  }
}
