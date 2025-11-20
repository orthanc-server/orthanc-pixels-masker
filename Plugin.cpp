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


#include <Logging.h>
#include <Toolbox.h>
#include <Compatibility.h>
#include <OrthancException.h>
#include "SystemToolbox.h"
#include "DicomParsing/ParsedDicomFile.h"
#include "DicomFormat/DicomPath.h"
#include "DicomParsing/FromDcmtkBridge.h"
#include "DicomFormat/DicomInstanceHasher.h"

#include <OrthancPluginCppWrapper.h>
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>
#include <json/value.h>
#include <string.h>
#include <iostream>
#include <algorithm>

// #if ORTHANC_STANDALONE == 1
// #  include <EmbeddedResources.h>
// #else
// #  include <SystemToolbox.h>
// #endif


// static void GetEmbeddedResource(std::string& target,
//                                 const std::string& path)
// {
// #if ORTHANC_STANDALONE == 0
//   Orthanc::SystemToolbox::ReadFile(target, Orthanc::SystemToolbox::InterpretRelativePath(PLUGIN_RESOURCES_PATH, path));
// #else
//   const std::string s = "/" + path;
//   Orthanc::EmbeddedResources::GetDirectoryResource(target, Orthanc::EmbeddedResources::PLUGIN_RESOURCES, s.c_str());
// #endif
// }
extern "C"
{

  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* c)
  {
    OrthancPlugins::SetGlobalContext(c, ORTHANC_PLUGIN_NAME);
    Orthanc::Logging::InitializePluginContext(c, ORTHANC_PLUGIN_NAME);
  
    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(c) == 0)
    {
      OrthancPlugins::ReportMinimalOrthancVersion(ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
                                                  ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
                                                  ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      return -1;
    }

    { // init the OrthancFramework
      static const char* const LOCALE = "Locale";
      static const char* const DEFAULT_ENCODING = "DefaultEncoding";

      /**
       * This function is a simplified version of function
       * "Orthanc::OrthancInitialize()" that is executed when starting the
       * Orthanc server.
       **/
      OrthancPlugins::OrthancConfiguration globalConfig;
      Orthanc::InitializeFramework(globalConfig.GetStringValue(LOCALE, ""), false /* loadPrivateDictionary */);

      std::string encoding;
      if (globalConfig.LookupStringValue(encoding, DEFAULT_ENCODING))
      {
        Orthanc::SetDefaultDicomEncoding(Orthanc::StringToEncoding(encoding.c_str()));
      }
      else
      {
        Orthanc::SetDefaultDicomEncoding(Orthanc::ORTHANC_DEFAULT_DICOM_ENCODING);
      }      
    }

    ORTHANC_PLUGINS_LOG_WARNING("Pixels masker plugin is initializing");
    OrthancPluginSetDescription2(c, ORTHANC_PLUGIN_NAME, "Expand Orthanc /modify & /anonymize REST API routes with pixels masking features.");

    // OrthancPluginRegisterRestCallback(OrthancPlugins::GetGlobalContext(), "/worklists/create", PostCreateWorklist);
    // OrthancPluginRegisterRestCallback(OrthancPlugins::GetGlobalContext(), "/worklists/([^/]+)", GetPutDeleteWorklist);

    return 0;
  }


  ORTHANC_PLUGINS_API void OrthancPluginFinalize()
  {
    ORTHANC_PLUGINS_LOG_WARNING("Pixels masker plugin is finalizing");
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
