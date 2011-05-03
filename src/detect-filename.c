/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 *
 */

#include "suricata-common.h"
#include "threads.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-spm-bm.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "app-layer.h"

#include "stream-tcp.h"

#include "detect-filename.h"

int DetectFilenameMatch (ThreadVars *, DetectEngineThreadCtx *, Flow *, uint8_t, void *, Signature *, SigMatch *);
static int DetectFilenameSetup (DetectEngineCtx *, Signature *, char *);
void DetectFilenameRegisterTests(void);
void DetectFilenameFree(void *);

/**
 * \brief Registration function for keyword: filename
 */
void DetectFilenameRegister(void) {
    sigmatch_table[DETECT_FILENAME].name = "filename";
    sigmatch_table[DETECT_FILENAME].Match = NULL;
    sigmatch_table[DETECT_FILENAME].AppLayerMatch = DetectFilenameMatch;
    sigmatch_table[DETECT_FILENAME].alproto = ALPROTO_HTTP;
    sigmatch_table[DETECT_FILENAME].Setup = DetectFilenameSetup;
    sigmatch_table[DETECT_FILENAME].Free  = DetectFilenameFree;
    sigmatch_table[DETECT_FILENAME].RegisterTests = DetectFilenameRegisterTests;

	SCLogDebug("registering filename rule option");
    return;
}

/**
 * \brief match the specified filename
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectFilenameData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectFilenameMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f, uint8_t flags, void *state, Signature *s, SigMatch *m)
{
    SCEnter();
    int ret = 0;

    DetectFilenameData *filename = m->ctx;

    SCMutexLock(&f->files_m);
    if (f->files != NULL) {
        FlowFile *file = f->files->head;
        for (; file != NULL; file = file->next) {
            if (file->state == FLOWFILE_STATE_NONE)
                continue;

            if (file->name == NULL)
                continue;

            if (BoyerMooreNocase(filename->name, filename->len, file->name,
                file->name_len, filename->bm_ctx->bmGs,
                filename->bm_ctx->bmBc) != NULL)
            {
#ifdef DEBUG
                if (SCLogDebugEnabled()) {
                    char *name = SCMalloc(filename->len + 1);
                    memcpy(name, filename->name, filename->len);
                    name[filename->len] = '\0';
                    SCLogDebug("will look for filename %s", name);
                }
#endif

                if (!(filename->flags & DETECT_CONTENT_NEGATED)) {
                    ret = 1;
                    /* Stop searching */
                    break;
                }
            }
        }

        if (ret == 0 && filename->flags & DETECT_CONTENT_NEGATED) {
            SCLogDebug("negated match");
            ret = 1;
        }
    }

    SCMutexUnlock(&f->files_m);
    SCReturnInt(ret);
}

/**
 * \brief Parse the filename keyword
 *
 * \param idstr Pointer to the user provided option
 *
 * \retval filename pointer to DetectFilenameData on success
 * \retval NULL on failure
 */
DetectFilenameData *DetectFilenameParse (char *str)
{
    DetectFilenameData *filename = NULL;

    /* We have a correct filename option */
    filename = SCMalloc(sizeof(DetectFilenameData));
    if (filename == NULL)
        goto error;

    memset(filename, 0x00, sizeof(DetectFilenameData));

    if (DetectParseContentString (str, &filename->name, &filename->len, &filename->flags) == -1) {
        goto error;
    }

    filename->bm_ctx = BoyerMooreCtxInit(filename->name, filename->len);
    if (filename->bm_ctx == NULL) {
        goto error;
    }

    SCLogDebug("flags %02X", filename->flags);
    if (filename->flags & DETECT_CONTENT_NEGATED) {
        SCLogDebug("negated filename");
    }

    BoyerMooreCtxToNocase(filename->bm_ctx, filename->name, filename->len);
#ifdef DEBUG
    if (SCLogDebugEnabled()) {
        char *name = SCMalloc(filename->len + 1);
        memcpy(name, filename->name, filename->len);
        name[filename->len] = '\0';
        SCLogDebug("will look for filename %s", name);
    }
#endif

    return filename;

error:
    if (filename != NULL)
        DetectFilenameFree(filename);
    return NULL;
}

/**
 * \brief this function is used to parse filename options
 * \brief into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param str pointer to the user provided "filename" option
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
static int DetectFilenameSetup (DetectEngineCtx *de_ctx, Signature *s, char *str)
{
    DetectFilenameData *filename = NULL;
    SigMatch *sm = NULL;

    filename = DetectFilenameParse(str);
    if (filename == NULL)
        goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_FILENAME;
    sm->ctx = (void *)filename;

    SigMatchAppendAppLayer(s, sm);

    if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_HTTP) {
        SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "rule contains conflicting keywords.");
        goto error;
    }

    AppLayerHtpNeedFileInspection();

    s->alproto = ALPROTO_HTTP;
    return 0;

error:
    if (filename != NULL)
        DetectFilenameFree(filename);
    if (sm != NULL)
        SCFree(sm);
    return -1;
}

/**
 * \brief this function will free memory associated with DetectFilenameData
 *
 * \param filename pointer to DetectFilenameData
 */
void DetectFilenameFree(void *ptr) {
    if (ptr != NULL) {
        DetectFilenameData *filename = (DetectFilenameData *)ptr;
        if (filename->bm_ctx != NULL) {
            BoyerMooreCtxDeInit(filename->bm_ctx);
        }
        SCFree(filename);
    }
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test DetectFilenameTestParse01
 */
int DetectFilenameTestParse01 (void) {
    DetectFilenameData *dnd = DetectFilenameParse("secret.pdf");
    if (dnd != NULL) {
        DetectFilenameFree(dnd);
        return 1;
    }
    return 0;
}

/**
 * \test DetectFilenameTestParse02
 */
int DetectFilenameTestParse02 (void) {
    int result = 0;

    DetectFilenameData *dnd = DetectFilenameParse("\"backup.tar.gz\"");
    if (dnd != NULL) {
        if (dnd->len == 13 && memcmp(dnd->name, "backup.tar.gz", 13) == 0) {
            result = 1;
        }

        DetectFilenameFree(dnd);
        return result;
    }
    return 0;
}

/**
 * \test DetectFilenameTestParse03
 */
int DetectFilenameTestParse03 (void) {
    int result = 0;

    DetectFilenameData *dnd = DetectFilenameParse("cmd.exe");
    if (dnd != NULL) {
        if (dnd->len == 7 && memcmp(dnd->name, "cmd.exe", 7) == 0) {
            result = 1;
        }

        DetectFilenameFree(dnd);
        return result;
    }
    return 0;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectFilename
 */
void DetectFilenameRegisterTests(void) {
#ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("DetectFilenameTestParse01", DetectFilenameTestParse01, 1);
    UtRegisterTest("DetectFilenameTestParse02", DetectFilenameTestParse02, 1);
    UtRegisterTest("DetectFilenameTestParse03", DetectFilenameTestParse03, 1);
#endif /* UNITTESTS */
}
