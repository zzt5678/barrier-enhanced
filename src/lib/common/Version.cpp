/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/Version.h"
#include "common/ProductIdentity.h"

const char* kApplication = WEAVE_PRODUCT_NAME;
const char* kCopyright   = "Copyright (C) 2018 Debauchee Open Source Group\n"
                           "Copyright (C) 2012-2016 Symless Ltd.\n"
                           "Copyright (C) 2008-2014 Nick Bolton\n"
                           "Copyright (C) 2002-2014 Chris Schoeneman";
const char* kContact     = "Issues: " WEAVE_SUPPORT_URL;
const char* kWebsite     = WEAVE_PROJECT_URL;
const char* kVersion     = BARRIER_VERSION;
const char* kAppVersion  = WEAVE_PRODUCT_NAME " " BARRIER_VERSION;
const char* kBuildRevision = BARRIER_REVISION;
const char* kBuildDate     = BARRIER_BUILD_DATE;
const char* kBuildId       = BARRIER_VERSION "+" BARRIER_REVISION "." BARRIER_BUILD_DATE;
