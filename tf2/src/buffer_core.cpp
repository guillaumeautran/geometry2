/*
 * Copyright (c) 2010, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \author Tully Foote */

#include "tf2/buffer_core.h"
#include "tf2/exceptions.h"
#include "tf2_msgs/TF2Error.h"
//legacy
#include "tf/tf.h"
#include "tf/transform_datatypes.h"


using namespace tf2;

// Must provide storage for non-integral static const class members.
// Otherwise you get undefined symbol errors on OS X (why not on Linux?).
// Thanks to Rob for pointing out the right way to do this.
const double tf2::BufferCore::DEFAULT_CACHE_TIME;

void setIdentity(geometry_msgs::Transform& tx)
{
  tx.translation.x = 0;
  tx.translation.y = 0;
  tx.translation.z = 0;
  tx.rotation.x = 0;
  tx.rotation.y = 0;
  tx.rotation.z = 0;
  tx.rotation.w = 1;
}

BufferCore::BufferCore(ros::Duration cache_time): old_tf_(true, cache_time)
{
  max_extrapolation_distance_.fromNSec(DEFAULT_MAX_EXTRAPOLATION_DISTANCE);
  frameIDs_["NO_PARENT"] = 0;
  frames_.push_back(NULL);// new TimeCache(interpolating, cache_time, max_extrapolation_distance));//unused but needed for iteration over all elements
  frameIDs_reverse.push_back("NO_PARENT");

  return;
}

BufferCore::~BufferCore()
{

}

void BufferCore::clear()
{
  old_tf_.clear();


  boost::mutex::scoped_lock(frame_mutex_);
  if ( frames_.size() > 1 )
  {
    for (std::vector< TimeCache*>::iterator  cache_it = frames_.begin() + 1; cache_it != frames_.end(); ++cache_it)
    {
      (*cache_it)->clearList();
    }
  }
  
}

bool BufferCore::setTransform(const geometry_msgs::TransformStamped& transform_in, const std::string& authority)
{

  /////BACKEARDS COMPATABILITY 
  tf::StampedTransform tf_transform;
  tf::transformStampedMsgToTF(transform_in, tf_transform);
  if  (!old_tf_.setTransform(tf_transform, authority))
  {
    printf("Warning old setTransform Failed but was not caught\n");
  }

  /////// New implementation
  bool error_exists = false;
  if (transform_in.child_frame_id == transform_in.header.frame_id)
  {
    ROS_ERROR("TF_SELF_TRANSFORM: Ignoring transform from authority \"%s\" with frame_id and child_frame_id  \"%s\" because they are the same",  authority.c_str(), transform_in.child_frame_id.c_str());
    error_exists = true;
  }

  if ((transform_in.child_frame_id == "/") | (transform_in.child_frame_id == ""))//empty frame id will be mapped to "/"
  {
    ROS_ERROR("TF_NO_CHILD_FRAME_ID: Ignoring transform from authority \"%s\" because child_frame_id not set ", authority.c_str());
    error_exists = true;
  }

  if ((transform_in.header.frame_id == "/") | (transform_in.header.frame_id == ""))//empty parent id will be mapped to "/"
  {
    ROS_ERROR("TF_NO_FRAME_ID: Ignoring transform with child_frame_id \"%s\"  from authority \"%s\" because frame_id not set", transform_in.child_frame_id.c_str(), authority.c_str());
    error_exists = true;
  }

  if (std::isnan(transform_in.transform.translation.x) || std::isnan(transform_in.transform.translation.y) || std::isnan(transform_in.transform.translation.z)||
      std::isnan(transform_in.transform.rotation.x) ||       std::isnan(transform_in.transform.rotation.y) ||       std::isnan(transform_in.transform.rotation.z) ||       std::isnan(transform_in.transform.rotation.w))
  {
    ROS_ERROR("TF_NAN_INPUT: Ignoring transform for child_frame_id \"%s\" from authority \"%s\" because of a nan value in the transform (%f %f %f) (%f %f %f %f)",
              transform_in.child_frame_id.c_str(), authority.c_str(),
              transform_in.transform.translation.x, transform_in.transform.translation.y, transform_in.transform.translation.z,
              transform_in.transform.rotation.x, transform_in.transform.rotation.y, transform_in.transform.rotation.z, transform_in.transform.rotation.w
              );
    error_exists = true;
  }

  if (error_exists)
    return false;
  unsigned int frame_number = lookupOrInsertFrameNumber(transform_in.child_frame_id);
  if (getFrame(frame_number)->insertData(TransformStorage(transform_in, lookupOrInsertFrameNumber(transform_in.header.frame_id))))
  {
    frame_authority_[frame_number] = authority;
  }
  else
  {
    ROS_WARN("TF_OLD_DATA ignoring data from the past for frame %s at time %g according to authority %s\nPossible reasons are listed at ", transform_in.child_frame_id.c_str(), transform_in.header.stamp.toSec(), authority.c_str());
    return false;
  }

  return true;

};


geometry_msgs::TransformStamped BufferCore::lookupTransform(const std::string& target_frame, 
                                                            const std::string& source_frame,
                                                            const ros::Time& time) const
{
  geometry_msgs::TransformStamped output_transform;

  // Short circuit if zero length transform to allow lookups on non existant links
  if (source_frame == target_frame)
  {
    setIdentity(output_transform.transform);


    output_transform.header.stamp = time;
    /*    if (time == ros::Time())
      output_transform.header.stamp = ros::Time(ros::TIME_MAX); ///\todo review what this should be
    else
      output_transform.header.stamp  = time;
    */

    output_transform.child_frame_id = source_frame;
    output_transform.header.frame_id = target_frame;
    return output_transform;
  }

  //  printf("Mapped Source: %s \nMapped Target: %s\n", source_frame.c_str(), target_frame.c_str());
  int retval = tf2_msgs::TF2Error::NO_ERROR;
  ros::Time temp_time;
  std::string error_string;
  //If getting the latest get the latest common time
  if (time == ros::Time())
    retval = getLatestCommonTime(target_frame, source_frame, temp_time, &error_string);
  else
    temp_time = time;

  TransformLists t_list;

  if (retval == tf2_msgs::TF2Error::NO_ERROR)
    try
    {
      retval = lookupLists(lookupFrameNumber( target_frame), temp_time, lookupFrameNumber( source_frame), t_list, &error_string);
    }
    catch (tf::LookupException &ex)
    {
      error_string = ex.what();
      retval = tf2_msgs::TF2Error::LOOKUP_ERROR;
    }
  if (retval != tf2_msgs::TF2Error::NO_ERROR)
  {
    std::stringstream ss;
    ss << " When trying to transform between " << source_frame << " and " << target_frame <<".";
    if (retval == tf2_msgs::TF2Error::LOOKUP_ERROR)
      throw LookupException(error_string + ss.str());
    if (retval == tf2_msgs::TF2Error::CONNECTIVITY_ERROR)
      throw ConnectivityException(error_string + ss.str());
  }

  if (test_extrapolation(temp_time, t_list, &error_string))
    {
    std::stringstream ss;
    if (time == ros::Time())// Using latest common time if we extrapolate this means that one of the links is out of date
    {
      ss << "Could not find a common time " << source_frame << " and " << target_frame <<".";
      throw ConnectivityException(ss.str());
    }
    else
    {
      ss << " When trying to transform between " << source_frame << " and " << target_frame <<"."<< std::endl;
      throw ExtrapolationException(error_string + ss.str());
    }
    }


  btTransform output = computeTransformFromList(t_list);
  tf2::transformTF2ToMsg(output, output_transform.transform);
  output_transform.header.stamp = temp_time;
  output_transform.header.frame_id = target_frame;
  output_transform.child_frame_id = source_frame;
  return output_transform;
};

                                                       
geometry_msgs::TransformStamped BufferCore::lookupTransform(const std::string& target_frame, 
                                                        const ros::Time& target_time,
                                                        const std::string& source_frame,
                                                        const ros::Time& source_time,
                                                        const std::string& fixed_frame) const
{

  geometry_msgs::TransformStamped output;
  geometry_msgs::TransformStamped temp1 =  lookupTransform(fixed_frame, source_frame, source_time);
  geometry_msgs::TransformStamped temp2 =  lookupTransform(target_frame, fixed_frame, target_time);
  
  btTransform bt1, bt2;
  tf2::transformMsgToTF2(temp1.transform, bt1);
  tf2::transformMsgToTF2(temp2.transform, bt2);
  tf2::transformTF2ToMsg(bt2*bt1, output.transform);
  output.header.stamp = temp2.header.stamp;
  output.header.frame_id = target_frame;
  output.child_frame_id = source_frame;
  return output;
};




geometry_msgs::Twist BufferCore::lookupTwist(const std::string& tracking_frame, 
                                          const std::string& observation_frame, 
                                          const ros::Time& time, 
                                          const ros::Duration& averaging_interval) const
{
  try
  {
  geometry_msgs::Twist t;
  old_tf_.lookupTwist(tracking_frame, observation_frame, 
                      time, averaging_interval, t);
  return t;
  }
  catch (tf::LookupException& ex)
  {
    throw tf2::LookupException(ex.what());
  }
  catch (tf::ConnectivityException& ex)
  {
    throw tf2::ConnectivityException(ex.what());
  }
  catch (tf::ExtrapolationException& ex)
  {
    throw tf2::ExtrapolationException(ex.what());
  }
  catch (tf::InvalidArgument& ex)
  {
    throw tf2::InvalidArgumentException(ex.what());
  }
};

geometry_msgs::Twist BufferCore::lookupTwist(const std::string& tracking_frame, 
                                          const std::string& observation_frame, 
                                          const std::string& reference_frame,
                                          const tf::Point & reference_point, 
                                          const std::string& reference_point_frame, 
                                          const ros::Time& time, 
                                          const ros::Duration& averaging_interval) const
{
  try{
  geometry_msgs::Twist t;
  old_tf_.lookupTwist(tracking_frame, observation_frame, reference_frame, reference_point, reference_point_frame,
                      time, averaging_interval, t);
  return t;
  }
  catch (tf::LookupException& ex)
  {
    throw tf2::LookupException(ex.what());
  }
  catch (tf::ConnectivityException& ex)
  {
    throw tf2::ConnectivityException(ex.what());
  }
  catch (tf::ExtrapolationException& ex)
  {
    throw tf2::ExtrapolationException(ex.what());
  }
  catch (tf::InvalidArgument& ex)
  {
    throw tf2::InvalidArgumentException(ex.what());
  }
};



bool BufferCore::canTransform(const std::string& target_frame, const std::string& source_frame,
                           const ros::Time& time, std::string* error_msg) const
{
  return old_tf_.canTransform(target_frame, source_frame, time, error_msg);
}

bool BufferCore::canTransform(const std::string& target_frame, const ros::Time& target_time,
                          const std::string& source_frame, const ros::Time& source_time,
                          const std::string& fixed_frame, std::string* error_msg) const
{
  return old_tf_.canTransform(target_frame, target_time, source_frame, source_time, fixed_frame, error_msg);
}


tf2::TimeCache* BufferCore::getFrame(unsigned int frame_id) const
{
  if (frame_id == 0) /// @todo check larger values too
    return NULL;
  else
    return frames_[frame_id];
};

unsigned int BufferCore::lookupFrameNumber(const std::string& frameid_str) const
{
  unsigned int retval = 0;
  boost::mutex::scoped_lock(frame_mutex_);
  std::map<std::string, unsigned int>::const_iterator map_it = frameIDs_.find(frameid_str);
  if (map_it == frameIDs_.end())
  {
    std::stringstream ss;
    ss << "Frame id " << frameid_str << " does not exist!";
    throw tf::LookupException(ss.str());
  }
  else
    retval = map_it->second;
  return retval;
};

unsigned int BufferCore::lookupOrInsertFrameNumber(const std::string& frameid_str)
{
  unsigned int retval = 0;
  boost::mutex::scoped_lock(frame_mutex_);
  std::map<std::string, unsigned int>::iterator map_it = frameIDs_.find(frameid_str);
  if (map_it == frameIDs_.end())
  {
    retval = frames_.size();
    frames_.push_back( new TimeCache(cache_time, max_extrapolation_distance_));
    frameIDs_[frameid_str] = retval;
    frameIDs_reverse.push_back(frameid_str);
  }
  else
    retval = frameIDs_[frameid_str];

  return retval;
};

std::string BufferCore::lookupFrameString(unsigned int frame_id_num) const
  {
    if (frame_id_num >= frameIDs_reverse.size())
    {
      std::stringstream ss;
      ss << "Reverse lookup of frame id " << frame_id_num << " failed!";
      throw tf2::LookupException(ss.str());
    }
    else
      return frameIDs_reverse[frame_id_num];

  };


int BufferCore::lookupLists(unsigned int target_frame, ros::Time time, unsigned int source_frame, TransformLists& lists, std::string * error_string) const
{
  /*  timeval tempt;
  gettimeofday(&tempt,NULL);
  std::cerr << "Looking up list at " <<tempt.tv_sec * 1000000ULL + tempt.tv_usec << std::endl;
  */

  ///\todo add fixed frame support

  //Clear lists before operating
  lists.forwardTransforms.clear();
  lists.inverseTransforms.clear();
  //  TransformLists mTfLs;
  if (target_frame == source_frame)
    return 0;  //Don't do anythign if we're not going anywhere

  TransformStorage temp;

  unsigned int frame = source_frame;
  unsigned int counter = 0;  //A counter to keep track of how deep we've descended
  unsigned int last_inverse;
  if (getFrame(frame) == NULL) //Test if source frame exists this will throw a lookup error if it does not (inside the loop it will be caught)
  {
    if (error_string) *error_string = "Source frame '"+lookupFrameString(frame)+"' does not exist is tf tree.";
    return tf2_msgs::TF2Error::LOOKUP_ERROR;//throw LookupException("Frame didn't exist");
  }
  while (true)
    {
      //      printf("getting data from %d:%s \n", frame, lookupFrameString(frame).c_str());

      TimeCache* pointer = getFrame(frame);
      ROS_ASSERT(pointer);

      if (! pointer->getData(time, temp))
      {
        last_inverse = frame;
        // this is thrown when there is no data
        break;
      }

      //break if parent is NO_PARENT (0)
      if (frame == 0)
      {
        last_inverse = frame;
        break;
      }
      lists.inverseTransforms.push_back(temp);

      frame = temp.frame_id_num_;


      /* Check if we've gone too deep.  A loop in the tree would cause this */
      if (counter++ > MAX_GRAPH_DEPTH)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss<<"The tf tree is invalid because it contains a loop." << std::endl
            << allFramesAsString() << std::endl;
          *error_string =ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;
        //        throw(LookupException(ss.str()));
      }
    }
  /*
    timeval tempt2;
  gettimeofday(&tempt2,NULL);
  std::cerr << "Side A " <<tempt.tv_sec * 1000000LL + tempt.tv_usec- tempt2.tv_sec * 1000000LL - tempt2.tv_usec << std::endl;
  */
  frame = target_frame;
  counter = 0;
  unsigned int last_forward;
  if (getFrame(frame) == NULL)
  {
    if (error_string) *error_string = "Target frame '"+lookupFrameString(frame)+"' does not exist is tf tree.";
    return tf2_msgs::TF2Error::LOOKUP_ERROR;
  }//throw LookupException("fixme");; //Test if source frame exists this will throw a lookup error if it does not (inside the loop it will be caught)
  while (true)
    {

      TimeCache* pointer = getFrame(frame);
      ROS_ASSERT(pointer);


      if(!  pointer->getData(time, temp))
      {
        last_forward = frame;
        break;
      }

      //break if parent is NO_PARENT (0)
      if (frame == 0)
      {
        last_forward = frame;
        break;
      }
      //      std::cout << "pushing back" << temp.frame_id_ << std::endl;
      lists.forwardTransforms.push_back(temp);
      frame = temp.frame_id_num_;

      /* Check if we've gone too deep.  A loop in the tree would cause this*/
      if (counter++ > MAX_GRAPH_DEPTH){
        if (error_string)
        {
          std::stringstream ss;
          ss<<"The tf tree is invalid because it contains a loop." << std::endl
            << allFramesAsString() << std::endl;
          *error_string = ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;//throw(LookupException(ss.str()));
      }
    }
  /*
  gettimeofday(&tempt2,NULL);
  std::cerr << "Side B " <<tempt.tv_sec * 1000000LL + tempt.tv_usec- tempt2.tv_sec * 1000000LL - tempt2.tv_usec << std::endl;
  */

  std::string connectivity_error("Could not find a connection between '"+lookupFrameString(target_frame)+"' and '"+
                                 lookupFrameString(source_frame)+"' because they are not part of the same tree."+
                                 "Tf has two or more unconnected trees.");
  /* Check the zero length cases*/
  if (lists.inverseTransforms.size() == 0)
  {
    if (lists.forwardTransforms.size() == 0) //If it's going to itself it's already been caught
    {
      if (error_string) *error_string = connectivity_error;
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }

    if (last_forward != source_frame)  //\todo match with case A
    {
      if (error_string) *error_string = connectivity_error;
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }
    else return 0;
  }

  if (lists.forwardTransforms.size() == 0)
  {
    if (lists.inverseTransforms.size() == 0)  //If it's going to itself it's already been caught
    {//\todo remove THis is the same as case D
      if (error_string) *error_string = connectivity_error;
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }

    try
    {
      if (lookupFrameNumber(lists.inverseTransforms.back().header.frame_id) != target_frame)
      {
        if (error_string) *error_string = connectivity_error;
        return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }
    else return 0;
    }
    catch (tf::LookupException & ex)
    {
      if (error_string) *error_string = ex.what();
      return tf2_msgs::TF2Error::LOOKUP_ERROR;
    }
  }


  /* Make sure the end of the search shares a parent. */
  if (last_forward != last_inverse)
  {
    if (error_string) *error_string = connectivity_error;
    return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
  }
  /* Make sure that we don't have a no parent at the top */
  try
  {
    if (lookupFrameNumber(lists.inverseTransforms.back().child_frame_id) == 0 || lookupFrameNumber( lists.forwardTransforms.back().child_frame_id) == 0)
    {
      //if (error_string) *error_string = "NO_PARENT at top of tree";
      if (error_string) *error_string = connectivity_error;
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }

    /*
      gettimeofday(&tempt2,NULL);
      std::cerr << "Base Cases done" <<tempt.tv_sec * 1000000LL + tempt.tv_usec- tempt2.tv_sec * 1000000LL - tempt2.tv_usec << std::endl;
    */

    while (lookupFrameNumber(lists.inverseTransforms.back().child_frame_id) == lookupFrameNumber(lists.forwardTransforms.back().child_frame_id))
    {
      lists.inverseTransforms.pop_back();
      lists.forwardTransforms.pop_back();

      // Make sure we don't go beyond the beginning of the list.
      // (The while statement above doesn't fail if you hit the beginning of the list,
      // which happens in the zero distance case.)
      if (lists.inverseTransforms.size() == 0 || lists.forwardTransforms.size() == 0)
	break;
    }
  }
  catch (tf::LookupException & ex)
  {
    if (error_string) *error_string = ex.what();
    return tf2_msgs::TF2Error::LOOKUP_ERROR;
  }  /*
       gettimeofday(&tempt2,NULL);
       std::cerr << "Done looking up list " <<tempt.tv_sec * 1000000LL + tempt.tv_usec- tempt2.tv_sec * 1000000LL - tempt2.tv_usec << std::endl;
     */
  return 0;

  }


bool BufferCore::test_extrapolation_one_value(const ros::Time& target_time, const TransformStorage& tr, std::string* error_string) const
{
  std::stringstream ss;
  ss << std::fixed;
  ss.precision(3);

  if (tr.mode_ == ONE_VALUE)
  {
    if (tr.header.stamp - target_time > max_extrapolation_distance_ || target_time - tr.header.stamp > max_extrapolation_distance_)
    {
      if (error_string) {
        ss << "You requested a transform at time " << (target_time).toSec() 
           << ",\n but the tf buffer only contains a single transform " 
           << "at time " << tr.header.stamp.toSec() << ".\n";
        if ( max_extrapolation_distance_ > ros::Duration(0))
        {
          ss << "The tf extrapollation distance is set to " 
             << (max_extrapolation_distance_).toSec() <<" seconds.\n";
        }
        *error_string = ss.str();
      }
      return true;
    }
  }
  return false;
}


bool BufferCore::test_extrapolation_past(const ros::Time& target_time, const TransformStorage& tr, std::string* error_string) const
{
  std::stringstream ss;
  ss << std::fixed;
  ss.precision(3);

  if (tr.mode_ == EXTRAPOLATE_BACK &&  tr.header.stamp - target_time > max_extrapolation_distance_)
  {
    if (error_string) {
      ss << "Extrapolating into the past.  You requested a transform at time " << target_time.toSec() << " seconds \n"
         << "but the tf buffer only has a history of until " << tr.header.stamp.toSec()  << " seconds.\n";
      if ( max_extrapolation_distance_ > ros::Duration(0))
      {
        ss << "The tf extrapollation distance is set to " 
           << (max_extrapolation_distance_).toSec() <<" seconds.\n";
      }
      *error_string = ss.str();
    }
    return true;
  }
  return false;
}


bool BufferCore::test_extrapolation_future(const ros::Time& target_time, const TransformStorage& tr, std::string* error_string) const
{
  std::stringstream ss;
  ss << std::fixed;
  ss.precision(3);

  if( tr.mode_ == EXTRAPOLATE_FORWARD && target_time - tr.header.stamp > max_extrapolation_distance_)
  {
    if (error_string){
      ss << "Extrapolating into the future.  You requested a transform that is at time" << target_time.toSec() << " seconds, \n"
         << "but the most recent transform in the tf buffer is at " << tr.header.stamp.toSec() << " seconds.\n";
      if ( max_extrapolation_distance_ > ros::Duration(0))
      {
        ss << "The tf extrapollation distance is set to " 
           << (max_extrapolation_distance_).toSec() <<" seconds.\n";
      }
      *error_string = ss.str();
    }
    return true;
  }
  return false;
}


bool BufferCore::test_extrapolation(const ros::Time& target_time, const TransformLists& lists, std::string * error_string) const
{
  std::stringstream ss;
  ss << std::fixed;
  ss.precision(3);
  for (unsigned int i = 0; i < lists.inverseTransforms.size(); i++)
  {
    if (test_extrapolation_one_value(target_time, lists.inverseTransforms[i], error_string)) return true;
    if (test_extrapolation_past(target_time, lists.inverseTransforms[i], error_string)) return true;
    if (test_extrapolation_future(target_time, lists.inverseTransforms[i], error_string)) return true;
  }

  for (unsigned int i = 0; i < lists.forwardTransforms.size(); i++)
  {
    if (test_extrapolation_one_value(target_time, lists.forwardTransforms[i], error_string)) return true;
    if (test_extrapolation_past(target_time, lists.forwardTransforms[i], error_string)) return true;
    if (test_extrapolation_future(target_time, lists.forwardTransforms[i], error_string)) return true;
  }

  return false;
}



btTransform BufferCore::computeTransformFromList(const TransformLists & lists) const
{
  btTransform retTrans;
  retTrans.setIdentity();
  ///@todo change these to iterators
  for (unsigned int i = 0; i < lists.inverseTransforms.size(); i++)
    {
      retTrans *= transformMsgToBT((lists.inverseTransforms[lists.inverseTransforms.size() -1 - i]).transform); //Reverse to get left multiply
    }
  for (unsigned int i = 0; i < lists.forwardTransforms.size(); i++)
    {
      retTrans = transformMsgToBT((lists.forwardTransforms[lists.forwardTransforms.size() -1 - i]).transform).inverse() * retTrans; //Do this list backwards(from backwards) for it was generated traveling the wrong way
    }

  return retTrans;
}

std::string BufferCore::allFramesAsString() const
{
  std::stringstream mstream;
  boost::mutex::scoped_lock(frame_mutex_);

  TransformStorage temp;



  //  for (std::vector< TimeCache*>::iterator  it = frames_.begin(); it != frames_.end(); ++it)
  for (unsigned int counter = 1; counter < frames_.size(); counter ++)
  {
    unsigned int frame_id_num;
    if(  getFrame(counter)->getData(ros::Time(), temp))
      frame_id_num = temp.frame_id_num_;
    else
    {
      frame_id_num = 0;
    }
    mstream << "Frame "<< frameIDs_reverse[counter] << " exists with parent " << frameIDs_reverse[frame_id_num] << "." <<std::endl;
  }
  return mstream.str();
}

int BufferCore::getLatestCommonTime(const std::string& source, const std::string& dest, ros::Time & time, std::string * error_string) const
{

  time = ros::Time(ros::TIME_MAX);
  int retval;
  TransformLists lists;
  try
  {
    retval = lookupLists(lookupFrameNumber(dest), ros::Time(), lookupFrameNumber(source), lists, error_string);
  }
  catch (tf::LookupException &ex)
  {
    time = ros::Time();
    if (error_string) *error_string = ex.what();
    return tf2_msgs::TF2Error::LOOKUP_ERROR;
  }
  if (retval == tf2_msgs::TF2Error::NO_ERROR)
  {
    //Set time to latest timestamp of frameid in case of target and source frame id are the same
    if (lists.inverseTransforms.size() == 0 && lists.forwardTransforms.size() == 0)
    {
      time = ros::Time(); ///\todo review was now();
      return retval;
    }

    for (unsigned int i = 0; i < lists.inverseTransforms.size(); i++)
    {
      if (time > lists.inverseTransforms[i].header.stamp)
        time = lists.inverseTransforms[i].header.stamp;
    }
    for (unsigned int i = 0; i < lists.forwardTransforms.size(); i++)
    {
      if (time > lists.forwardTransforms[i].header.stamp)
        time = lists.forwardTransforms[i].header.stamp;
    }

  }
  else
    time.fromSec(0);

  return retval;
};
