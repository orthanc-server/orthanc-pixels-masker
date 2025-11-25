/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2023 Osimis S.A., Belgium
 * Copyright (C) 2024-2025 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2025 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "PixelsMaskerJob.h"

#include "DicomPixelsMasker.h"

#include <SerializationToolbox.h>


static void GetInstances(std::vector<std::string>& instances,
                         Orthanc::ResourceType level,
                         std::string resourceId)
{
  std::string base;
  switch (level)
  {
    case Orthanc::ResourceType_Patient:
      base = "/patients/";
      break;

    case Orthanc::ResourceType_Study:
      base = "/studies/";
      break;

    case Orthanc::ResourceType_Series:
      base = "/series/";
      break;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
    
  Json::Value json;
  if (!OrthancPlugins::RestApiGet(json, base + resourceId + "/instances", false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
  }

  if (json.type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  instances.resize(json.size());
  
  for (Json::Value::ArrayIndex i = 0; i < json.size(); i++)
  {
    instances[i] = Orthanc::SerializationToolbox::ReadString(json[i], "ID");
  }
}


void PixelsMaskerJob::ApplyToDicomInstance(IDicomConsumer& consumer,
                                           const std::string& instanceId)
{
  std::string file;
  if (!OrthancPlugins::RestApiGetString(file, "/instances/" + instanceId + "/file", false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "Instance has been removed: " + instanceId);
  }

  std::unique_ptr<Orthanc::ParsedDicomFile> dicom(new Orthanc::ParsedDicomFile(file));
  modification_->Apply(dicom);

  dicom->SaveToMemoryBuffer(file);

  Json::Value answer;
  if (transcode_)
  {
    // We use Orthanc's transcoding facilities to take advantage of transcoding plugins (GDCM)
    std::unique_ptr<OrthancPlugins::DicomInstance> transcoded(OrthancPlugins::DicomInstance::Transcode(file.empty() ? NULL : file.c_str(), file.size(), targetSyntax_));

    consumer.Consume(transcoded->GetBuffer(), transcoded->GetSize());
  }
  else
  {
    consumer.Consume(file.empty() ? NULL : file.c_str(), file.size());
  }
}


PixelsMaskerJob::PixelsMaskerJob(Orthanc::DicomModification* modification,
                                 Orthanc::ResourceType level,
                                 const std::string& resourceId,
                                 const Json::Value& body) :
  OrthancJob("PixelsMasker"),
  modification_(modification),
  transcode_(false),
  keepSource_(true),
  current_(0)
{
  static const char* const TRANSCODE = "Transcode";
  static const char* const KEEP_SOURCE = "KeepSource";

  if (modification == NULL)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NullPointer);
  }

  if (body.isMember(TRANSCODE))
  {
    transcode_ = true;
    targetSyntax_ = Orthanc::SerializationToolbox::ReadString(body, TRANSCODE);

    Orthanc::DicomTransferSyntax t;
    if (!Orthanc::LookupTransferSyntax(t, targetSyntax_))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, "Unknown transfer syntax: " + targetSyntax_);
    }
  }

  if (body.isMember(KEEP_SOURCE))
  {
    keepSource_ = Orthanc::SerializationToolbox::ReadBoolean(body, KEEP_SOURCE);
  }

  {
    std::unique_ptr<DicomPixelsMasker> masker(new DicomPixelsMasker);
    masker->ParseRequest(body);
    modification_->SetDicomModifier(masker.release());
  }

  if (level == Orthanc::ResourceType_Instance)
  {
    instances_.push_back(resourceId);
  }
  else
  {
    GetInstances(instances_, level, resourceId);
  }
}


OrthancPluginJobStepStatus PixelsMaskerJob::Step()
{
  class UploadConsumer : public IDicomConsumer
  {
  private:
    Json::Value answer_;
    
  public:
    virtual void Consume(const void* dicom,
                         size_t size) ORTHANC_OVERRIDE
    {
      if (!OrthancPlugins::RestApiPost(answer_, "/instances", dicom, size, false))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "Cannot upload modified instance");
      }
    }

    std::string GetUploadedId() const
    {
      return Orthanc::SerializationToolbox::ReadString(answer_, "ID");
    }
  };
        
  try
  {
    if (current_ == 0)
    {
      ClearContent();
    }

    if (current_ == instances_.size())
    {
      if (!keepSource_)
      {
        for (size_t i = 0; i < instances_.size(); i++)
        {
          OrthancPlugins::RestApiDelete("/instances/" + instances_[i], false);
        }
      }

      UpdateProgress(1);
      
      return OrthancPluginJobStepStatus_Success;
    }
    else
    {
      UpdateProgress(static_cast<float>(current_) / static_cast<float>(instances_.size()));
      
      const std::string sourceId = instances_[current_];

      UploadConsumer consumer;
      ApplyToDicomInstance(consumer, sourceId);

      const std::string uploadedId = consumer.GetUploadedId();
      const std::string metadata = (modification_->IsAnonymization() ? "AnonymizedFrom" : "ModifiedFrom");

      Json::Value answer;
      if (!OrthancPlugins::RestApiPut(answer, "/instances/" + uploadedId + "/metadata/" + metadata, sourceId, false))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "Cannot set metadata: " + metadata);
      }

      current_++;

      return OrthancPluginJobStepStatus_Continue;
    }
  }
  catch (Orthanc::OrthancException& e)
  {
    Json::Value info;
    info["Error"] = e.What();
    info["ErrorDetails"] = e.GetDetails();
    UpdateContent(info);
  }

  return OrthancPluginJobStepStatus_Failure;
}
    

void PixelsMaskerJob::Reset()
{
  current_ = 0;
}


void PixelsMaskerJob::ApplyToDicomInstance(std::string& modifiedDicom,
                                           const std::string& instanceId)
{
  class StringConsumer : public IDicomConsumer
  {
  private:
    bool         first_;
    std::string& target_;
    
  public:
    StringConsumer(std::string& target) :
      first_(true),
      target_(target)
    {
    }
    
    virtual void Consume(const void* dicom,
                         size_t size) ORTHANC_OVERRIDE
    {
      if (first_)
      {
        first_ = false;
        target_.assign(reinterpret_cast<const char*>(dicom), size);
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadSequenceOfCalls);
      }
    }
  };

  StringConsumer consumer(modifiedDicom);
  ApplyToDicomInstance(consumer, instanceId);
}
