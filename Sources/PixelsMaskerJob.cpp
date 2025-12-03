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
#include <Logging.h>


static std::unique_ptr<OrthancPlugins::OrthancConfiguration> globalConfiguration_;


static std::string GetBasePath(Orthanc::ResourceType level)
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

    case Orthanc::ResourceType_Instance:
      base = "/instances/";
      break;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
  return base;
}

static void GetInstances(std::vector<std::string>& instances,
                         Orthanc::ResourceType level,
                         std::string resourceId)
{
  if (level == Orthanc::ResourceType_Instance)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  Json::Value json;
  if (!OrthancPlugins::RestApiGet(json, GetBasePath(level) + resourceId + "/instances", false))
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

static void GetLabels(std::set<std::string>& labels,
                      Orthanc::ResourceType level,
                      std::string resourceId)
{
  Json::Value jsonLabels;

  if (!OrthancPlugins::RestApiGet(jsonLabels, GetBasePath(level) + resourceId+ "/labels", false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "Resource has been removed : " + GetBasePath(level) + resourceId);
  }
  Orthanc::SerializationToolbox::ReadSetOfStrings(labels, jsonLabels);
}

static void SetLabels(const std::set<std::string>& labels,
                      Orthanc::ResourceType level,
                      std::string resourceId)
{
  Json::Value result;
  Json::Value emptyPayload;

  for (std::set<std::string>::const_iterator it = labels.begin(); it != labels.end(); ++it)
  {
    if (!OrthancPlugins::RestApiPut(result, GetBasePath(level) + resourceId + "/labels/" + *it, emptyPayload, false))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "Failed to set label to : " + GetBasePath(level) + resourceId);
    }
  }
}




void PixelsMaskerJob::ApplyToDicomInstance(IDicomConsumer& consumer,
                                           const std::string& instanceId)
{
  std::set<std::string> instanceLabels;
  std::set<std::string> seriesLabels;
  std::set<std::string> studyLabels;
  std::set<std::string> patientLabels;

  std::string file;
  if (!OrthancPlugins::RestApiGetString(file, "/instances/" + instanceId + "/file", false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "Instance has been removed: " + instanceId);
  }

  std::unique_ptr<Orthanc::ParsedDicomFile> dicom(new Orthanc::ParsedDicomFile(file));

  if (keepLabels_)
  {
    Orthanc::DicomInstanceHasher originalHasher(dicom->GetHasher());

    GetLabels(instanceLabels, Orthanc::ResourceType_Instance, instanceId);
    GetLabels(seriesLabels, Orthanc::ResourceType_Series, originalHasher.HashSeries());
    GetLabels(studyLabels, Orthanc::ResourceType_Study, originalHasher.HashStudy());
    GetLabels(patientLabels, Orthanc::ResourceType_Patient, originalHasher.HashPatient());
  }

  modification_->Apply(dicom);

  dicom->SaveToMemoryBuffer(file);

  std::unique_ptr<Orthanc::DicomInstanceHasher> modifiedHasher;

  Json::Value answer;
  if (transcode_)
  {
    // We use Orthanc's transcoding facilities to take advantage of transcoding plugins (GDCM)
    std::unique_ptr<OrthancPlugins::DicomInstance> transcoded(OrthancPlugins::DicomInstance::Transcode(file.empty() ? NULL : file.c_str(), file.size(), targetSyntax_));

    consumer.Consume(transcoded->GetBuffer(), transcoded->GetSize());

    if (keepLabels_)
    {
      std::unique_ptr<Orthanc::ParsedDicomFile> transcodedParsedDicomFile(new Orthanc::ParsedDicomFile(transcoded->GetBuffer(), transcoded->GetSize()));
      modifiedHasher.reset(new Orthanc::DicomInstanceHasher(transcodedParsedDicomFile->GetHasher()));
    }
  }
  else
  {
    consumer.Consume(file.empty() ? NULL : file.c_str(), file.size());

    if (keepLabels_)
    {
      modifiedHasher.reset(new Orthanc::DicomInstanceHasher(dicom->GetHasher()));
    }
  }

  if (keepLabels_)
  {
    SetLabels(instanceLabels, Orthanc::ResourceType_Instance, modifiedHasher->HashInstance());
    SetLabels(seriesLabels, Orthanc::ResourceType_Series, modifiedHasher->HashSeries());
    SetLabels(studyLabels, Orthanc::ResourceType_Study, modifiedHasher->HashStudy());
    SetLabels(patientLabels, Orthanc::ResourceType_Patient, modifiedHasher->HashPatient());
  }
}


PixelsMaskerJob::PixelsMaskerJob(Orthanc::DicomModification* modification,
                                 Orthanc::ResourceType level,
                                 const std::string& resourceId,
                                 const Json::Value& body,
                                 size_t workerThreadsCount) :
  OrthancJob("PixelsMasker"),
  modification_(modification),
  transcode_(false),
  keepSource_(true),
  keepLabels_(false),
  current_(0),
  workerThreadsCount_(workerThreadsCount),
  workersShouldStop_(false),
  instancesToProcess_(workerThreadsCount)
{
  static const char* const TRANSCODE = "Transcode";
  static const char* const KEEP_SOURCE = "KeepSource";
  static const char* const KEEP_LABELS = "KeepLabels";

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

  if (body.isMember(KEEP_LABELS))
  {
    keepLabels_ = Orthanc::SerializationToolbox::ReadBoolean(body, KEEP_LABELS);
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

static boost::mutex modifierThreadsCounterMutex;
static uint32_t modifierThreadsCounter = 0;

class UploadConsumer : public PixelsMaskerJob::IDicomConsumer
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


void PixelsMaskerJob::ModifierWorkerThread(PixelsMaskerJob* that)
{
  {
    boost::mutex::scoped_lock lock(modifierThreadsCounterMutex);
    Orthanc::Logging::SetCurrentThreadName(std::string("PIXM-MODI-") + boost::lexical_cast<std::string>(modifierThreadsCounter++));
    modifierThreadsCounter %= 1000000;
  }

  while (true)
  {
    std::unique_ptr<Orthanc::SingleValueObject<std::string> > instanceToProcess(dynamic_cast<Orthanc::SingleValueObject<std::string>*>(that->instancesToProcess_.Dequeue(0)));
    if (instanceToProcess.get() == NULL || that->workersShouldStop_)  // that's the signal to exit the thread
    {
      LOG(INFO) << "Modifier thread has completed";
      return;
    }
    
    try
    {
      UploadConsumer consumer;
      std::string sourceId = instanceToProcess->GetValue();

      that->ApplyToDicomInstance(consumer, sourceId);

      const std::string uploadedId = consumer.GetUploadedId();
      const std::string metadata = (that->modification_->IsAnonymization() ? "AnonymizedFrom" : "ModifiedFrom");

      Json::Value answer;
      if (!OrthancPlugins::RestApiPut(answer, "/instances/" + uploadedId + "/metadata/" + metadata, sourceId, false))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "Cannot set metadata: " + metadata);
      }
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Error while modifying instances " << e.GetDetails();
    }
    catch (...)
    {
      LOG(ERROR) << "Unknown error while modifying instances ";
    }
  }
}


OrthancPluginJobStepStatus PixelsMaskerJob::Step()
{
  try
  {
    if (current_ == 0) // first step
    {
      ClearContent();

      for (size_t i = 0; i < workerThreadsCount_; i++)
      {
        workerThreads_.push_back(new boost::thread(ModifierWorkerThread, this));
      }

    }

    if (current_ == instances_.size()) // last step
    {
      if (!keepSource_)
      {
        for (size_t i = 0; i < instances_.size(); i++)
        {
          OrthancPlugins::RestApiDelete("/instances/" + instances_[i], false);
        }
      }

      UpdateProgress(1);
      ClearThreads();
      
      return OrthancPluginJobStepStatus_Success;
    }
    else
    {
      UpdateProgress(static_cast<float>(current_) / static_cast<float>(instances_.size()));

      // enqueue the instances to process one by one.  The flow is controlled by the BlockingSharedMessageQueue that has one slot for each worker
      instancesToProcess_.Enqueue(new Orthanc::SingleValueObject<std::string>(instances_[current_]));

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
  ClearThreads();
  workersShouldStop_ = false;
}

void PixelsMaskerJob::ClearThreads()
{
  workersShouldStop_ = true;

  for (size_t i = 0; i < workerThreadsCount_; i++)
  {
    instancesToProcess_.Enqueue(NULL); // that's the stop signal !
  }
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
