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

#include <Logging.h>
#include <OrthancPluginCppWrapper.h>

#include <SerializationToolbox.h>
#include <DicomParsing/DicomModification.h>
#include <DicomParsing/ParsedDicomFile.h>

static const int32_t GlobalProperty_AnonymizationSequence = 4655;  // TODO - Document this in the Orthanc Book

static std::string defaultPrivateCreator_;


static void ParseModifyRequest(Json::Value& body,
                               Orthanc::DicomModification& modification,
                               const OrthancPluginHttpRequest* request)
{
  if (!Orthanc::Toolbox::ReadJson(body, request->body, request->bodySize))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }
  else
  {
    modification.SetAllowManualIdentifiers(true);
    modification.SetPrivateCreator(defaultPrivateCreator_);  
    modification.ParseModifyRequest(body);
  }
}


static void ParseAnonymizationRequest(Json::Value& body,
                                      Orthanc::DicomModification& modification,
                                      const OrthancPluginHttpRequest* request)
{
  if (!Orthanc::Toolbox::ReadJson(body, request->body, request->bodySize))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }
  else
  {
    modification.SetPrivateCreator(defaultPrivateCreator_);  

    bool patientNameOverridden;
    modification.ParseAnonymizationRequest(patientNameOverridden, body);

    if (!patientNameOverridden)
    {
      // Override the random Patient's Name by one that is more
      // user-friendly (provided none was specified by the user)

      // NB: There could be a race condition here, but it is not really important
      OrthancPlugins::OrthancString tmp;
      tmp.Assign(OrthancPluginGetGlobalProperty(OrthancPlugins::GetGlobalContext(), GlobalProperty_AnonymizationSequence, ""));

      std::string s;
      tmp.ToString(s);

      int64_t value;
      if (!Orthanc::SerializationToolbox::ParseInteger64(value, s))
      {
        value = 0;
      }

      s = boost::lexical_cast<std::string>(value + 1);
      OrthancPluginSetGlobalProperty(OrthancPlugins::GetGlobalContext(), GlobalProperty_AnonymizationSequence, s.c_str());      
      modification.Replace(Orthanc::DICOM_TAG_PATIENT_NAME, "Anonymized" + s, true);
    }
  }
}


template <Orthanc::ResourceType level, bool isAnonymization>
static void ModifyResource(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "POST");
  }
  else
  {
    assert(request->groupsCount == 1);
    const std::string resourceId(request->groups[0]);
    
    Json::Value body;
    std::unique_ptr<Orthanc::DicomModification> modification(new Orthanc::DicomModification);

    if (isAnonymization)
    {
      ParseAnonymizationRequest(body, *modification, request);
    }
    else
    {
      ParseModifyRequest(body, *modification, request);
    }

    std::unique_ptr<PixelsMaskerJob> job(new PixelsMaskerJob(modification.release(), level, resourceId, body));

    Json::Value answer = Json::objectValue;
    OrthancPlugins::OrthancJob::SubmitFromRestApiPost(output, answer, job.release());
  }
}


template <bool isAnonymization>
static void ModifyInstance(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "POST");
  }
  else
  {
    assert(request->groupsCount == 1);
    const std::string instanceId(request->groups[0]);
    
    Json::Value body;
    std::unique_ptr<Orthanc::DicomModification> modification(new Orthanc::DicomModification);

    if (isAnonymization)
    {
      ParseAnonymizationRequest(body, *modification, request);
    }
    else
    {
      ParseModifyRequest(body, *modification, request);
    }

    PixelsMaskerJob job(modification.release(), Orthanc::ResourceType_Instance, instanceId, body);

    std::string modified;
    job.ApplyToDicomInstance(modified, instanceId);
    
    OrthancPluginAnswerBuffer(OrthancPlugins::GetGlobalContext(), output, modified.empty() ? NULL : modified.c_str(),
                              modified.size(), Orthanc::EnumerationToString(Orthanc::MimeType_Dicom));
  }
}


static bool DisplayPerformanceWarning()
{
  (void) DisplayPerformanceWarning;   // Disable warning about unused function
  LOG(WARNING) << "Performance warning in plugin: "
               << "Non-release build, runtime debug assertions are turned on";
  return true;
}


extern "C"
{
  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    OrthancPlugins::SetGlobalContext(context, ORTHANC_PLUGIN_NAME);

#if ORTHANC_FRAMEWORK_VERSION_IS_ABOVE(1, 12, 4)
    Orthanc::Logging::InitializePluginContext(context, ORTHANC_PLUGIN_NAME);
#elif ORTHANC_FRAMEWORK_VERSION_IS_ABOVE(1, 7, 2)
    Orthanc::Logging::InitializePluginContext(context);
#else
    Orthanc::Logging::Initialize(context);
#endif

    assert(DisplayPerformanceWarning());

    Orthanc::Logging::EnableInfoLevel(true);

    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(context) == 0)
    {
      char info[1024];
      sprintf(info, "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
              context->orthancVersion,
              ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      OrthancPluginLogError(context, info);
      return -1;
    }

    OrthancPlugins::SetDescription(ORTHANC_PLUGIN_NAME, "Pixel masker plugin for Orthanc.");

    try
    {
      OrthancPlugins::OrthancConfiguration configuration;
      defaultPrivateCreator_ = configuration.GetStringValue("DefaultPrivateCreator", "");
      
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Patient, false> >("/plugins/pixels-masker/patients/([0-9a-f-]+)/modify", true);
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Study, false> >("/plugins/pixels-masker/studies/([0-9a-f-]+)/modify", true);
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Series, false> >("/plugins/pixels-masker/series/([0-9a-f-]+)/modify", true);
      
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Patient, true> >("/plugins/pixels-masker/patients/([0-9a-f-]+)/anonymize", true);
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Study, true> >("/plugins/pixels-masker/studies/([0-9a-f-]+)/anonymize", true);
      OrthancPlugins::RegisterRestCallback< ModifyResource<Orthanc::ResourceType_Series, true> >("/plugins/pixels-masker/series/([0-9a-f-]+)/anonymize", true);

      OrthancPlugins::RegisterRestCallback< ModifyInstance<false> >("/plugins/pixels-masker/instances/([0-9a-f-]+)/modify", true);
      OrthancPlugins::RegisterRestCallback< ModifyInstance<true> >("/plugins/pixels-masker/instances/([0-9a-f-]+)/anonymize", true);
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Exception while initializing the plugin: " << e.What();
      return -1;
    }

    return 0;
  }


  ORTHANC_PLUGINS_API void OrthancPluginFinalize()
  {
    LOG(WARNING) << "Finalizing the pixel masker plugin";
    Orthanc::Logging::Finalize();
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
  {
    return ORTHANC_PLUGIN_NAME;
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
  {
    return ORTHANC_PLUGIN_VERSION;
  }
}
