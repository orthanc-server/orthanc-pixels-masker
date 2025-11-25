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


#pragma once

#include <DicomParsing/DicomModification.h>
#include <OrthancPluginCppWrapper.h>

class PixelsMaskerJob : public OrthancPlugins::OrthancJob
{
private:
  std::unique_ptr<Orthanc::DicomModification>  modification_;
  bool                                         transcode_;
  std::string                                  targetSyntax_;
  bool                                         keepSource_;
  std::vector<std::string>                     instances_;
  size_t                                       current_;

  class IDicomConsumer : public boost::noncopyable
  {
  public:
    virtual ~IDicomConsumer()
    {
    }

    virtual void Consume(const void* dicom,
                         size_t size) = 0;
  };

  void ApplyToDicomInstance(IDicomConsumer& consumer,
                            const std::string& instanceId);

public:
  PixelsMaskerJob(Orthanc::DicomModification* modification,
                  Orthanc::ResourceType level,
                  const std::string& resourceId,
                  const Json::Value& body);

  virtual OrthancPluginJobStepStatus Step() ORTHANC_OVERRIDE;

  virtual void Stop(OrthancPluginJobStopReason reason) ORTHANC_OVERRIDE
  {
  }    

  virtual void Reset() ORTHANC_OVERRIDE;

  void ApplyToDicomInstance(std::string& modifiedDicom,
                            const std::string& instanceId);
};
