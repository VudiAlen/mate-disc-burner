/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Librejilla-media
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-media authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-media. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-media is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>

#include "scsi-sbc.h"

#include "scsi-error.h"
#include "scsi-utils.h"
#include "scsi-base.h"
#include "scsi-command.h"
#include "scsi-opcodes.h"

#if G_BYTE_ORDER == G_LITTLE_ENDIAN

struct _RejillaScsiPreventAllowMediumRemovalUnitCDB {
	uchar opcode;

	uchar res1			[3];

	uchar prevent			:2;
	uchar res4			:6;

	uchar ctl;
};

#else

struct _RejillaScsiPreventAllowMediumRemovalUnitCDB {
	uchar opcode;

	uchar res1			[3];

	uchar res4			:6;
	uchar prevent			:2;

	uchar ctl;
};

#endif

typedef struct _RejillaScsiPreventAllowMediumRemovalUnitCDB RejillaScsiPreventAllowMediumRemovalUnitCDB;

REJILLA_SCSI_COMMAND_DEFINE (RejillaScsiPreventAllowMediumRemovalUnitCDB,
			     PREVENT_ALLOW_MEDIUM_REMOVAL,
			     REJILLA_SCSI_READ);

RejillaScsiResult
rejilla_sbc_medium_removal (RejillaDeviceHandle *handle,
                            int prevent_removal,
                            RejillaScsiErrCode *error)
{
	RejillaScsiPreventAllowMediumRemovalUnitCDB *cdb;
	RejillaScsiResult res;

	g_return_val_if_fail (handle != NULL, REJILLA_SCSI_FAILURE);

	cdb = rejilla_scsi_command_new (&info, handle);
	cdb->prevent = prevent_removal;
	res = rejilla_scsi_command_issue_sync (cdb,
					       NULL,
					       0,
					       error);
	rejilla_scsi_command_free (cdb);
	return res;
}
