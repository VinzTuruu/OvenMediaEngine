//==============================================================================
//
//  Provider Base Class 
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "provider.h"
#include "application.h"
#include "stream.h"
#include "provider_private.h"

namespace pvd
{
	StreamMotor::StreamMotor(uint32_t id)
	{
		_id = id;
	}

	uint32_t StreamMotor::GetId()
	{
		return _id;
	}

	bool StreamMotor::Start()
	{
		_stop_thread_flag = false;
		_thread = std::thread(&StreamMotor::WorkerThread, this);
		return true;
	}

	bool StreamMotor::Stop()
	{
		_stop_thread_flag = true;
		if(_thread.joinable())
		{
			_thread.join();
		}

		return true;
	}

	bool StreamMotor::AddStream(const std::shared_ptr<Stream> &stream)
	{
		std::unique_lock<std::shared_mutex> lock(_streams_map_guard);
		_streams[stream->GetId()] = stream;
		lock.unlock();

		logti("%s/%s(%u) stream has added to %u StreamMotor", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId(), GetId());

		stream->Play();

		return true;
	}

	bool StreamMotor::DelStream(const std::shared_ptr<Stream> &stream)
	{
		std::unique_lock<std::shared_mutex> lock(_streams_map_guard);
		if(_streams.find(stream->GetId()) == _streams.end())
		{
			return false;
		}
		_streams.erase(stream->GetId());
		lock.unlock();

		logti("%s/%s(%u) stream has deleted from %u StreamMotor", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId(), GetId());

		stream->Stop();

		return true;
	}

	void StreamMotor::WorkerThread()
	{
		
		while(true)
		{
			// TODO (Getroot): If there is no stream, use semaphore to wait until the stream is added.

			if(_stop_thread_flag)
			{
				break;
			}

			{
				std::shared_lock<std::shared_mutex> stream_lock(_streams_map_guard);
				
				for(auto &it : _streams)
				{
					auto stream = it.second;

					if(stream->GetState() == Stream::State::PLAYING)
					{
						auto result = stream->ProcessMediaPacket();
						if(result == Stream::ProcessMediaResult::PROCESS_MEDIA_SUCCESS || result == Stream::ProcessMediaResult::PROCESS_MEDIA_TRY_AGAIN)
						{

						}
						else
						{
							// remove stream
						}
					}
				}
			}
		}
	}


	Application::Application(const std::shared_ptr<Provider> &provider, const info::Application &application_info)
		: info::Application(application_info)
	{
		_provider = provider;
		_last_issued_stream_id = 100;
	}

	Application::~Application()
	{
		Stop();
	}

	const char* Application::GetApplicationTypeName()
	{
		if(_provider == nullptr)
		{
			return "";
		}

		if(_app_type_name.IsEmpty())
		{
			_app_type_name.Format("%s %s",  _provider->GetProviderName(), "Application");
		}

		return _app_type_name.CStr();
	}

	bool Application::Start()
	{
		logti("%s has created [%s] application", GetApplicationTypeName(), GetName().CStr());

		// Start to the white elephant streams colletor, it only works with pull provider
		if(_provider->GetProviderStreamDirection() == ProviderStreamDirection::Pull)
		{
			_stop_collector_thread_flag = false;
			_collector_thread = std::thread(&Application::WhiteElephantStreamCollector, this);
			return true;
		}

		_state = ApplicationState::Started;

		return true;
	}

	bool Application::Stop()
	{
		if(_state == ApplicationState::Stopped)
		{
			return true;
		}

		_stop_collector_thread_flag = true;
		if(_collector_thread.joinable())
		{
			_collector_thread.join();
		}

		DeleteAllStreams();
		logti("%s has deleted [%s] application", GetApplicationTypeName(), GetName().CStr());
		_state = ApplicationState::Stopped;
		return true;
	}

	// It works only with pull provider
	void Application::WhiteElephantStreamCollector()
	{
		while(!_stop_collector_thread_flag)
		{
			// TODO (Getroot): If there is no stream, use semaphore to wait until the stream is added.
			std::shared_lock<std::shared_mutex> lock(_streams_guard, std::defer_lock);
			lock.lock();
			// Copy to prevent performance degradation due to mutex lock in loop
			auto streams = _streams;
			lock.unlock();

			for(auto const &x : streams)
			{
				auto stream = x.second;
			
				if(stream->GetState() == Stream::State::STOPPED || stream->GetState() == Stream::State::ERROR)
				{
					DeleteStream(stream);
				}
				else if(stream->GetState() != Stream::State::STOPPING)
				{
					// Check if there are streams have no any viewers
					auto stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(stream));
					if(stream_metrics != nullptr)
					{
						auto current = std::chrono::high_resolution_clock::now();
						auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(current - stream_metrics->GetLastSentTime()).count();
						
						if(elapsed_time > MAX_UNUSED_STREAM_AVAILABLE_TIME_SEC)
						{
							logti("%s/%s(%u) stream will be deleted becasue it hasn't been used for %u seconds", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId(), MAX_UNUSED_STREAM_AVAILABLE_TIME_SEC);
							DeleteStream(stream);
						}
					}
				}
			}

			sleep(3);
		}
	}

	info::stream_id_t Application::IssueUniqueStreamId()
	{
		return _last_issued_stream_id++;
	}

	const std::shared_ptr<Stream> Application::GetStreamById(uint32_t stream_id)
	{
		std::shared_lock<std::shared_mutex> lock(_streams_guard);

		if(_streams.find(stream_id) == _streams.end())
		{
			return nullptr;
		}

		return _streams.at(stream_id);
	}

	const std::shared_ptr<Stream> Application::GetStreamByName(ov::String stream_name)
	{
		std::shared_lock<std::shared_mutex> lock(_streams_guard);
		
		for(auto const &x : _streams)
		{
			auto& stream = x.second;
			if(stream->GetName() == stream_name)
			{
				return stream;
			}
		}

		return nullptr;
	}

	uint32_t Application::GetStreamMotorId(const std::shared_ptr<Stream> &stream)
	{
		uint32_t hash = stream->GetId() % MAX_STREAM_MOTOR_COUNT;

		return hash;
	}

	// For push providers
	std::shared_ptr<pvd::Stream> Application::CreateStream(const uint32_t stream_id, const ov::String &stream_name, const std::vector<std::shared_ptr<MediaTrack>> &tracks)
	{
		auto stream = CreatePushStream(stream_id, stream_name);
		if(stream == nullptr)
		{
			return nullptr;
		}

		for(const auto &track : tracks)
		{
			stream->AddTrack(track);
		}
	
		std::unique_lock<std::shared_mutex> streams_lock(_streams_guard);
		_streams[stream->GetId()] = stream;
		streams_lock.unlock();

		NotifyStreamCreated(stream);

		return stream;
	}

	std::shared_ptr<pvd::Stream> Application::CreateStream(const ov::String &stream_name, const std::vector<std::shared_ptr<MediaTrack>> &tracks)
	{
		return CreateStream(IssueUniqueStreamId(), stream_name, tracks);
	}

	std::shared_ptr<StreamMotor> Application::CreateStreamMotorInternal(const std::shared_ptr<Stream> &stream)
	{
		auto motor_id = GetStreamMotorId(stream);
		auto motor = std::make_shared<StreamMotor>(motor_id);

		_stream_motors.emplace(motor_id, motor);
		motor->Start();

		logti("%s application has created %u stream motor", stream->GetApplicationInfo().GetName().CStr(), motor_id);

		return motor;
	}

	// For pull providers
	std::shared_ptr<pvd::Stream> Application::CreateStream(const ov::String &stream_name, const std::vector<ov::String> &url_list)
	{
		auto stream = CreatePullStream(IssueUniqueStreamId(), stream_name, url_list);
		if(stream == nullptr)
		{
			return nullptr;
		}

		std::unique_lock<std::shared_mutex> streams_lock(_streams_guard);
		
		_streams[stream->GetId()] = stream;
		auto motor = GetStreamMotorInternal(stream);
		if(motor == nullptr)
		{
			motor = CreateStreamMotorInternal(stream);
			if(motor == nullptr)
			{
				logtc("Cannot create StreamMotor : %s/%s(%u)", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId());
				return nullptr;
			}
		}

		streams_lock.unlock();

		// Create stream first
		NotifyStreamCreated(stream);
		// And push data next
		motor->AddStream(stream);
		

		return stream;
	}

	bool Application::DeleteStream(const std::shared_ptr<Stream> &stream)
	{
		std::unique_lock<std::shared_mutex> streams_lock(_streams_guard);

		DeleteStreamInternal(stream);

		streams_lock.unlock();
		
		NotifyStreamDeleted(stream);

		return true;
	}

	bool Application::DeleteStreamInternal(const std::shared_ptr<Stream> &stream)
	{
		if(_streams.find(stream->GetId()) == _streams.end())
		{
			logtc("Could not find stream to be removed : %s/%s(%u)", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId());
			return false;
		}
		_streams.erase(stream->GetId());

		auto motor = GetStreamMotorInternal(stream);
		if(motor == nullptr)
		{
			logtc("Could not find stream motor to remove stream : %s/%s(%u)", stream->GetApplicationInfo().GetName().CStr(), stream->GetName().CStr(), stream->GetId());
			return false;
		}
		
		motor->DelStream(stream);

		//TODO(Getroot): If there is no stream in the StreamMotor, delete StreamMotor to save system resource.

		return true;
	}

	std::shared_ptr<StreamMotor> Application::GetStreamMotorInternal(const std::shared_ptr<Stream> &stream)
	{
		std::shared_ptr<StreamMotor> motor = nullptr;;
		if(_provider->GetProviderStreamDirection() == ProviderStreamDirection::Pull)
		{
			auto motor_id = GetStreamMotorId(stream);
			auto it = _stream_motors.find(motor_id);
			if(it == _stream_motors.end())
			{
				logtd("Could not find stream motor : %s/%s(%u)", GetName().CStr(), stream->GetName().CStr(), stream->GetId());
				return nullptr;
			}

			motor = it->second;
		}

		return motor;
	}

	bool Application::NotifyStreamCreated(std::shared_ptr<Stream> stream)
	{
		MediaRouteApplicationConnector::CreateStream(stream);
		return true;
	}

	bool Application::NotifyStreamDeleted(std::shared_ptr<Stream> stream)
	{
		MediaRouteApplicationConnector::DeleteStream(stream);
		return true;
	}

	bool Application::DeleteAllStreams()
	{
		std::unique_lock<std::shared_mutex> lock(_streams_guard);

		for(auto it = _streams.cbegin(); it != _streams.cend(); )
		{
			auto stream = it->second;

			stream->Stop();
			MediaRouteApplicationConnector::DeleteStream(stream);
			it = _streams.erase(it);
		}

		return true;
	}
}