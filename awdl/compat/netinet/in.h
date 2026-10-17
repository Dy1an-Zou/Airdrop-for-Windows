/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OWL only needs struct in6_addr from <netinet/in.h>. */
#pragma once
#ifdef _MSC_VER
#include <winsock2.h>
#include <in6addr.h>
#else
#include <in6addr.h>
#endif
