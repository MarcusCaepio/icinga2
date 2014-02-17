/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-present Icinga Development Team (http://www.icinga.org) *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "compat/checkresultreader.h"
#include "icinga/service.h"
#include "icinga/pluginutility.h"
#include "icinga/icingaapplication.h"
#include "base/dynamictype.h"
#include "base/objectlock.h"
#include "base/logger_fwd.h"
#include "base/convert.h"
#include "base/application.h"
#include "base/utility.h"
#include "base/exception.h"
#include "base/context.h"
#include "base/statsfunction.h"
#include <fstream>

using namespace icinga;

REGISTER_TYPE(CheckResultReader);

REGISTER_STATSFUNCTION(CheckResultReaderStats, &CheckResultReader::StatsFunc);

Value CheckResultReader::StatsFunc(Dictionary::Ptr& status, Dictionary::Ptr& perfdata)
{
	/* FIXME */
	status->Set("checkresultreader_", 1);

	return 0;
}

/**
 * @threadsafety Always.
 */
void CheckResultReader::Start(void)
{
	m_ReadTimer = make_shared<Timer>();
	m_ReadTimer->OnTimerExpired.connect(boost::bind(&CheckResultReader::ReadTimerHandler, this));
	m_ReadTimer->SetInterval(5);
	m_ReadTimer->Start();
}

/**
 * @threadsafety Always.
 */
void CheckResultReader::ReadTimerHandler(void) const
{
	CONTEXT("Processing check result files in '" + GetSpoolDir() + "'");

	Utility::Glob(GetSpoolDir() + "/c??????.ok", boost::bind(&CheckResultReader::ProcessCheckResultFile, this, _1));
}

void CheckResultReader::ProcessCheckResultFile(const String& path) const
{
	CONTEXT("Processing check result file '" + path + "'");

	String crfile = String(path.Begin(), path.End() - 3); /* Remove the ".ok" extension. */

	std::ifstream fp;
	fp.exceptions(std::ifstream::badbit);
	fp.open(crfile.CStr());

	std::map<String, String> attrs;

	while (fp.good()) {
		std::string line;
		std::getline(fp, line);

		if (line.empty() || line[0] == '#')
			continue; /* Ignore comments and empty lines. */

		size_t pos = line.find_first_of('=');

		if (pos == std::string::npos)
			continue; /* Ignore invalid lines. */

		String key = line.substr(0, pos);
		String value = line.substr(pos + 1);

		attrs[key] = value;
	}

	/* Remove the checkresult files. */
	if (unlink(path.CStr()) < 0)
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("unlink")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(path));

	if (unlink(crfile.CStr()) < 0)
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("unlink")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(crfile));

	Host::Ptr host = Host::GetByName(attrs["host_name"]);

	if (!host) {
		Log(LogWarning, "compat", "Ignoring checkresult file for host '" + attrs["host_name"] +
		    "': Host does not exist.");

		return;
	}

	Service::Ptr service = host->GetServiceByShortName(attrs["service_description"]);

	if (!service) {
		Log(LogWarning, "compat", "Ignoring checkresult file for host '" + attrs["host_name"] +
		    "', service '" + attrs["service_description"] + "': Service does not exist.");

		return;
	}

	CheckResult::Ptr result = PluginUtility::ParseCheckOutput(attrs["output"]);
	result->SetState(PluginUtility::ExitStatusToState(Convert::ToLong(attrs["return_code"])));
	result->SetExecutionStart(Convert::ToDouble(attrs["start_time"]));
	result->SetExecutionEnd(Convert::ToDouble(attrs["finish_time"]));
	result->SetCheckSource(IcingaApplication::GetInstance()->GetNodeName());

	service->ProcessCheckResult(result);

	Log(LogDebug, "compat", "Processed checkresult file for host '" + attrs["host_name"] +
		    "', service '" + attrs["service_description"] + "'");

	{
		ObjectLock olock(service);

		/* Reschedule the next check. The side effect of this is that for as long
		 * as we receive check result files for a service we won't execute any
		 * active checks. */
		service->SetNextCheck(Utility::GetTime() + service->GetCheckInterval());
	}
}
