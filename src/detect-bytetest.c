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
 * \author Brian Rectanus <brectanu@gmail.com>
 *
 * Implements byte_test keyword.
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"
#include "detect-engine.h"
#include "detect-parse.h"

#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-bytetest.h"
#include "detect-bytejump.h"
#include "detect-byte-extract.h"
#include "app-layer.h"

#include "util-byte.h"
#include "util-unittest.h"
#include "util-debug.h"
#include "detect-pcre.h"


/**
 * \brief Regex for parsing our options
 */
/** \todo We probably just need a simple tokenizer here */
#define PARSE_REGEX  "^\\s*" \
                     "([^\\s,]+)" \
                     "\\s*,\\s*(\\!?)\\s*([^\\s,]*)" \
                     "\\s*,\\s*([^\\s,]+)" \
                     "\\s*,\\s*([^\\s,]+)" \
                     "(?:\\s*,\\s*([^\\s,]+))?" \
                     "(?:\\s*,\\s*([^\\s,]+))?" \
                     "(?:\\s*,\\s*([^\\s,]+))?" \
                     "(?:\\s*,\\s*([^\\s,]+))?" \
                     "(?:\\s*,\\s*([^\\s,]+))?" \
                     "\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

void DetectBytetestRegisterTests(void);

void DetectBytetestRegister (void)
{
    const char *eb;
    int eo;
    int opts = 0;

    sigmatch_table[DETECT_BYTETEST].name = "byte_test";
    sigmatch_table[DETECT_BYTETEST].Match = DetectBytetestMatch;
    sigmatch_table[DETECT_BYTETEST].Setup = DetectBytetestSetup;
    sigmatch_table[DETECT_BYTETEST].Free  = DetectBytetestFree;
    sigmatch_table[DETECT_BYTETEST].RegisterTests = DetectBytetestRegisterTests;

    sigmatch_table[DETECT_BYTETEST].flags |= SIGMATCH_PAYLOAD;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at "
                   "offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/** \brief Bytetest detection code
 *
 *  Byte test works on the packet payload.
 *
 *  \param det_ctx thread de ctx
 *  \param s signature
 *  \param m sigmatch for this bytettest
 *  \param payload ptr to the start of the buffer to inspect
 *  \param payload_len length of the payload
 *  \retval 1 match
 *  \retval 0 no match
 */
int DetectBytetestDoMatch(DetectEngineThreadCtx *det_ctx, Signature *s, const SigMatchCtx *ctx, uint8_t *payload, uint32_t payload_len,
                          uint8_t flags, int32_t offset, uint64_t value)
{
    SCEnter();

    const DetectBytetestData *data = (const DetectBytetestData *)ctx;
    uint8_t *ptr = NULL;
    int32_t len = 0;
    uint64_t val = 0;
    int extbytes;
    int neg;
    int match;

    if (payload_len == 0) {
        SCReturnInt(0);
    }

    /* Calculate the ptr value for the bytetest and length remaining in
     * the packet from that point.
     */
    if (flags & DETECT_BYTETEST_RELATIVE) {
        SCLogDebug("relative, working with det_ctx->buffer_offset %"PRIu32", "
                   "data->offset %"PRIi32"", det_ctx->buffer_offset, data->offset);

        ptr = payload + det_ctx->buffer_offset;
        len = payload_len - det_ctx->buffer_offset;

        ptr += offset;
        len -= offset;

        /* No match if there is no relative base */
        if (ptr == NULL || len <= 0) {
            SCReturnInt(0);
        }
        //PrintRawDataFp(stdout,ptr,len);
    }
    else {
        SCLogDebug("absolute, data->offset %"PRIi32"", data->offset);

        ptr = payload + offset;
        len = payload_len - offset;
    }

    /* Validate that the to-be-extracted is within the packet
     * \todo Should this validate it is in the *payload*?
     */
    if (ptr < payload || data->nbytes > len) {
        SCLogDebug("Data not within payload pkt=%p, ptr=%p, len=%"PRIu32", nbytes=%d",
                    payload, ptr, len, data->nbytes);
        SCReturnInt(0);
    }

    neg = flags & DETECT_BYTETEST_NEGOP;

    /* Extract the byte data */
    if (flags & DETECT_BYTETEST_STRING) {
        extbytes = ByteExtractStringUint64(&val, data->base,
                                           data->nbytes, (const char *)ptr);
        if (extbytes <= 0) {
            /* strtoull() return 0 if there is no numeric value in data string */
            if (val == 0) {
                SCLogDebug("No Numeric value");
                SCReturnInt(0);
            } else {
                SCLogError(SC_ERR_INVALID_NUM_BYTES, "Error extracting %d "
                        "bytes of string data: %d", data->nbytes, extbytes);
                SCReturnInt(-1);
            }
        }

        SCLogDebug("comparing base %d string 0x%" PRIx64 " %s%c 0x%" PRIx64 "",
               data->base, val, (neg ? "!" : ""), data->op, data->value);
    }
    else {
        int endianness = (flags & DETECT_BYTETEST_LITTLE) ?
                          BYTE_LITTLE_ENDIAN : BYTE_BIG_ENDIAN;
        extbytes = ByteExtractUint64(&val, endianness, data->nbytes, ptr);
        if (extbytes != data->nbytes) {
            SCLogError(SC_ERR_INVALID_NUM_BYTES, "Error extracting %d bytes "
                   "of numeric data: %d\n", data->nbytes, extbytes);
            SCReturnInt(-1);
        }

        SCLogDebug("comparing numeric 0x%" PRIx64 " %s%c 0x%" PRIx64 "",
               val, (neg ? "!" : ""), data->op, data->value);
    }

    /* Compare using the configured operator */
    match = 0;
    switch (data->op) {
        case DETECT_BYTETEST_OP_EQ:
            if (val == value) {
                match = 1;
            }
            break;
        case DETECT_BYTETEST_OP_LT:
            if (val < value) {
                match = 1;
            }
            break;
        case DETECT_BYTETEST_OP_GT:
            if (val > value) {
                match = 1;
            }
            break;
        case DETECT_BYTETEST_OP_AND:
            if (val & value) {
                match = 1;
            }
            break;
        case DETECT_BYTETEST_OP_OR:
            if (val ^ value) {
                match = 1;
            }
            break;
        case DETECT_BYTETEST_OP_GE:
        if (val >= value) {
            match = 1;
        }
        break;
        case DETECT_BYTETEST_OP_LE:
        if (val <= value) {
            match = 1;
        }
        break;
        default:
            /* Should never get here as we handle this in parsing. */
            SCReturnInt(-1);
    }

    /* A successful match depends on negation */
    if ((!neg && match) || (neg && !match)) {
        SCLogDebug("MATCH");
        SCReturnInt(1);
    }

    SCLogDebug("NO MATCH");
    SCReturnInt(0);

}

int DetectBytetestMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx,
                        Packet *p, Signature *s, const SigMatchCtx *ctx)
{
    return DetectBytetestDoMatch(det_ctx, s, ctx, p->payload, p->payload_len,
                                 ((DetectBytetestData *)ctx)->flags, 0, 0);
}

DetectBytetestData *DetectBytetestParse(char *optstr, char **value, char **offset)
{
    DetectBytetestData *data = NULL;
    char *args[9] = {
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL
    };
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    int i;
    uint32_t nbytes;
    const char *str_ptr = NULL;

    /* Execute the regex and populate args with captures. */
    ret = pcre_exec(parse_regex, parse_regex_study, optstr,
                    strlen(optstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 6 || ret > 10) {
        SCLogError(SC_ERR_PCRE_PARSE, "parse error, ret %" PRId32
               ", string %s", ret, optstr);
        goto error;
    }
    for (i = 0; i < (ret - 1); i++) {
        res = pcre_get_substring((char *)optstr, ov, MAX_SUBSTRINGS,
                                 i + 1, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                   "for arg %d", i + 1);
            goto error;
        }
        args[i] = (char *)str_ptr;
    }

    /* Initialize the data */
    data = SCMalloc(sizeof(DetectBytetestData));
    if (unlikely(data == NULL))
        goto error;
    data->base = DETECT_BYTETEST_BASE_UNSET;
    data->flags = 0;


    /*
     * The first four options are required and positional.  The
     * remaining arguments are flags and are not positional.
     */

    /* Number of bytes */
    if (ByteExtractStringUint32(&nbytes, 10, 0, args[0]) <= 0) {
        SCLogError(SC_ERR_INVALID_VALUE, "Malformed number of bytes: %s", str_ptr);
        goto error;
    }

    /* Operator is next two args: neg + op */
    data->op = 0;
    if (args[1] != NULL && *args[1] == '!') {
        data->flags |= DETECT_BYTETEST_NEGOP;
    }

    if (args[2] != NULL) {
        if ((strcmp("=", args[2]) == 0) || ((data->flags & DETECT_BYTETEST_NEGOP)
                && strcmp("", args[2]) == 0)) {
            data->op |= DETECT_BYTETEST_OP_EQ;
        } else if (strcmp("<", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_LT;
        } else if (strcmp(">", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_GT;
        } else if (strcmp("&", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_AND;
        } else if (strcmp("^", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_OR;
        } else if (strcmp(">=", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_GE;
        } else if (strcmp("<=", args[2]) == 0) {
            data->op |= DETECT_BYTETEST_OP_LE;
        } else {
            SCLogError(SC_ERR_INVALID_OPERATOR, "Invalid operator");
            goto error;
        }
    }

    /* Value */
    if (args[3][0] != '-' && isalpha((unsigned char)args[3][0])) {
        if (value == NULL) {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "byte_test supplied with "
                       "var name for value.  \"value\" argument supplied to "
                       "this function has to be non-NULL");
            goto error;
        }
        *value = SCStrdup(args[3]);
        if (*value == NULL)
            goto error;
    } else {
        if (ByteExtractStringUint64(&data->value, 0, 0, args[3]) <= 0) {
            SCLogError(SC_ERR_INVALID_VALUE, "Malformed value: %s", str_ptr);
            goto error;
        }
    }

    /* Offset */
    if (args[4][0] != '-' && isalpha((unsigned char)args[4][0])) {
        if (offset == NULL) {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "byte_test supplied with "
                       "var name for offset.  \"offset\" argument supplied to "
                       "this function has to be non-NULL");
            goto error;
        }
        *offset = SCStrdup(args[4]);
        if (*offset == NULL)
            goto error;
    } else {
        if (ByteExtractStringInt32(&data->offset, 0, 0, args[4]) <= 0) {
            SCLogError(SC_ERR_INVALID_VALUE, " Malformed offset: %s", str_ptr);
            goto error;
        }
    }

    /* The remaining options are flags. */
    /** \todo Error on dups? */
    for (i = 5; i < (ret - 1); i++) {
        if (args[i] != NULL) {
            if (strcmp("relative", args[i]) == 0) {
                data->flags |= DETECT_BYTETEST_RELATIVE;
            } else if (strcasecmp("string", args[i]) == 0) {
                data->flags |= DETECT_BYTETEST_STRING;
            } else if (strcasecmp("dec", args[i]) == 0) {
                data->base |= DETECT_BYTETEST_BASE_DEC;
            } else if (strcasecmp("hex", args[i]) == 0) {
                data->base |= DETECT_BYTETEST_BASE_HEX;
            } else if (strcasecmp("oct", args[i]) == 0) {
                data->base |= DETECT_BYTETEST_BASE_OCT;
            } else if (strcasecmp("big", args[i]) == 0) {
                if (data->flags & DETECT_BYTETEST_LITTLE) {
                    data->flags ^= DETECT_BYTETEST_LITTLE;
                }
                data->flags |= DETECT_BYTETEST_BIG;
            } else if (strcasecmp("little", args[i]) == 0) {
                data->flags |= DETECT_BYTETEST_LITTLE;
            } else if (strcasecmp("dce", args[i]) == 0) {
                data->flags |= DETECT_BYTETEST_DCE;
            } else {
                SCLogError(SC_ERR_UNKNOWN_VALUE, "Unknown value: \"%s\"",
                        args[i]);
                goto error;
            }
        }
    }

    if (data->flags & DETECT_BYTETEST_STRING) {
        /* 23 - This is the largest string (octal, with a zero prefix) that
         *      will not overflow uint64_t.  The only way this length
         *      could be over 23 and still not overflow is if it were zero
         *      prefixed and we only support 1 byte of zero prefix for octal.
         *
         * "01777777777777777777777" = 0xffffffffffffffff
         */
        if (nbytes > 23) {
            SCLogError(SC_ERR_INVALID_VALUE, "Cannot test more than 23 bytes with \"string\": %s",
                        optstr);
            goto error;
        }
    } else {
        if (nbytes > 8) {
            SCLogError(SC_ERR_INVALID_VALUE, "Cannot test more than 8 bytes without \"string\": %s",
                        optstr);
            goto error;
        }
        if (data->base != DETECT_BYTETEST_BASE_UNSET) {
            SCLogError(SC_ERR_INVALID_VALUE, "Cannot use a base without \"string\": %s", optstr);
            goto error;
        }
    }

    /* This is max 23 so it will fit in a byte (see above) */
    data->nbytes = (uint8_t)nbytes;

    for (i = 0; i < (ret - 1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    return data;

error:
    for (i = 0; i < (ret - 1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    if (data != NULL) DetectBytetestFree(data);
    return NULL;
}

int DetectBytetestSetup(DetectEngineCtx *de_ctx, Signature *s, char *optstr)
{
    SigMatch *sm = NULL;
    SigMatch *prev_pm = NULL;
    DetectBytetestData *data = NULL;
    char *value = NULL;
    char *offset = NULL;
    int ret = -1;

    data = DetectBytetestParse(optstr, &value, &offset);
    if (data == NULL)
        goto error;

    int sm_list;
    if (s->list != DETECT_SM_LIST_NOTSET) {
        if (s->list == DETECT_SM_LIST_FILEDATA) {
            if (data->flags & DETECT_BYTETEST_DCE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "dce bytetest specified "
                           "with file_data option set.");
                goto error;
            }
            AppLayerHtpEnableResponseBodyCallback();
        }
        sm_list = s->list;
        s->flags |= SIG_FLAG_APPLAYER;
        if (data->flags & DETECT_BYTETEST_RELATIVE) {
            prev_pm = SigMatchGetLastSMFromLists(s, 4,
                                                 DETECT_CONTENT, s->sm_lists_tail[sm_list],
                                                 DETECT_PCRE, s->sm_lists_tail[sm_list]);
        }

    } else if (data->flags & DETECT_BYTETEST_DCE) {
        if (data->flags & DETECT_BYTETEST_RELATIVE) {
            prev_pm = SigMatchGetLastSMFromLists(s, 12,
                                                 DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH]);
            if (prev_pm == NULL) {
                sm_list = DETECT_SM_LIST_PMATCH;
            } else {
                sm_list = SigMatchListSMBelongsTo(s, prev_pm);
                if (sm_list < 0)
                    goto error;
            }
        } else {
            sm_list = DETECT_SM_LIST_PMATCH;
        }

        s->alproto = ALPROTO_DCERPC;
        s->flags |= SIG_FLAG_APPLAYER;

    } else if (data->flags & DETECT_BYTETEST_RELATIVE) {
        prev_pm = SigMatchGetLastSMFromLists(s, 168,
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_FILEDATA],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH]);
        if (prev_pm == NULL) {
            sm_list = DETECT_SM_LIST_PMATCH;
        } else {
            sm_list = SigMatchListSMBelongsTo(s, prev_pm);
            if (sm_list < 0)
                goto error;
        }

    } else {
        sm_list = DETECT_SM_LIST_PMATCH;
    }

    if (data->flags & DETECT_BYTETEST_DCE) {
        if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_DCERPC) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Non dce alproto sig has "
                       "bytetest with dce enabled");
            goto error;
        }
        if ((data->flags & DETECT_BYTETEST_STRING) ||
            (data->flags & DETECT_BYTETEST_LITTLE) ||
            (data->flags & DETECT_BYTETEST_BIG) ||
            (data->base == DETECT_BYTETEST_BASE_DEC) ||
            (data->base == DETECT_BYTETEST_BASE_HEX) ||
            (data->base == DETECT_BYTETEST_BASE_OCT) ) {
            SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "Invalid option. "
                       "A byte_test keyword with dce holds other invalid modifiers.");
            goto error;
        }
    }

    if (value != NULL) {
        SigMatch *bed_sm = DetectByteExtractRetrieveSMVar(value, s);
        if (bed_sm == NULL) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Unknown byte_extract var "
                       "seen in byte_test - %s\n", value);
            goto error;
        }
        data->value = ((DetectByteExtractData *)bed_sm->ctx)->local_id;
        data->flags |= DETECT_BYTETEST_VALUE_BE;
        SCFree(value);
    }

    if (offset != NULL) {
        SigMatch *bed_sm = DetectByteExtractRetrieveSMVar(offset, s);
        if (bed_sm == NULL) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Unknown byte_extract var "
                       "seen in byte_test - %s\n", offset);
            goto error;
        }
        data->offset = ((DetectByteExtractData *)bed_sm->ctx)->local_id;
        data->flags |= DETECT_BYTETEST_OFFSET_BE;
        SCFree(offset);
    }

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;
    sm->type = DETECT_BYTETEST;
    sm->ctx = (SigMatchCtx *)data;
    SigMatchAppendSMToList(s, sm, sm_list);

    if (!(data->flags & DETECT_BYTETEST_RELATIVE))
        goto okay;

    if (prev_pm == NULL)
        goto okay;
    if (prev_pm->type == DETECT_CONTENT) {
        DetectContentData *cd = (DetectContentData *)prev_pm->ctx;
        cd->flags |= DETECT_CONTENT_RELATIVE_NEXT;
    } else if (prev_pm->type == DETECT_PCRE) {
        DetectPcreData *pd = (DetectPcreData *)prev_pm->ctx;
        pd->flags |= DETECT_PCRE_RELATIVE_NEXT;
    }

 okay:
    ret = 0;
    return ret;
 error:
    DetectBytetestFree(data);
    return ret;
}

/**
 * \brief this function will free memory associated with DetectBytetestData
 *
 * \param data pointer to DetectBytetestData
 */
void DetectBytetestFree(void *ptr)
{
    if (ptr == NULL)
        return;

    DetectBytetestData *data = (DetectBytetestData *)ptr;
    SCFree(data);
}


/* UNITTESTS */
#ifdef UNITTESTS
#include "util-unittest-helper.h"
/**
 * \test DetectBytetestTestParse01 is a test to make sure that we return "something"
 *  when given valid bytetest opt
 */
int DetectBytetestTestParse01(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, =, 1 , 0", NULL, NULL);
    if (data != NULL) {
        DetectBytetestFree(data);
        result = 1;
    }

    return result;
}

/**
 * \test DetectBytetestTestParse02 is a test for setting the required opts
 */
int DetectBytetestTestParse02(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, !=, 1, 0", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 4)
            && (data->value == 1)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_NEGOP)
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse03 is a test for setting the relative flag
 */
int DetectBytetestTestParse03(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, !=, 1, 0, relative", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 4)
            && (data->value == 1)
            && (data->offset == 0)
            && (data->flags == ( DETECT_BYTETEST_NEGOP
                                |DETECT_BYTETEST_RELATIVE))
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse04 is a test for setting the string/oct flags
 */
int DetectBytetestTestParse04(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, !=, 1, 0, string, oct", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 4)
            && (data->value == 1)
            && (data->offset == 0)
            && (data->flags == ( DETECT_BYTETEST_NEGOP
                                |DETECT_BYTETEST_STRING))
            && (data->base == DETECT_BYTETEST_BASE_OCT))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse05 is a test for setting the string/dec flags
 */
int DetectBytetestTestParse05(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, =, 1, 0, string, dec", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 4)
            && (data->value == 1)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_STRING)
            && (data->base == DETECT_BYTETEST_BASE_DEC))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse06 is a test for setting the string/hex flags
 */
int DetectBytetestTestParse06(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, >, 1, 0, string, hex", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_GT)
            && (data->nbytes == 4)
            && (data->value == 1)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_STRING)
            && (data->base == DETECT_BYTETEST_BASE_HEX))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse07 is a test for setting the big flag
 */
int DetectBytetestTestParse07(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, <, 5, 0, big", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_LT)
            && (data->nbytes == 4)
            && (data->value == 5)
            && (data->offset == 0)
            && (data->flags == 4)
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse08 is a test for setting the little flag
 */
int DetectBytetestTestParse08(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, <, 5, 0, little", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_LT)
            && (data->nbytes == 4)
            && (data->value == 5)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_LITTLE)
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse09 is a test for neg operator only
 */
int DetectBytetestTestParse09(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, !, 5, 0", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 4)
            && (data->value == 5)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_NEGOP)
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse10 is a test for whitespace
 */
int DetectBytetestTestParse10(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("	4 , ! &, 5	, 0 , little ", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_AND)
            && (data->nbytes == 4)
            && (data->value == 5)
            && (data->offset == 0)
            && (data->flags == (DETECT_BYTETEST_NEGOP|DETECT_BYTETEST_LITTLE))
            && (data->base == DETECT_BYTETEST_BASE_UNSET))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse11 is a test for whitespace
 */
int DetectBytetestTestParse11(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4,!^,5,0,little,string,relative,hex", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_OR)
            && (data->nbytes == 4)
            && (data->value == 5)
            && (data->offset == 0)
            && (data->flags == ( DETECT_BYTETEST_NEGOP
                                |DETECT_BYTETEST_LITTLE
                                |DETECT_BYTETEST_STRING
                                |DETECT_BYTETEST_RELATIVE))
            && (data->base == DETECT_BYTETEST_BASE_HEX))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse12 is a test for hex w/o string
 */
int DetectBytetestTestParse12(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, =, 1, 0, hex", NULL, NULL);
    if (data == NULL) {
        result = 1;
    }

    return result;
}

/**
 * \test DetectBytetestTestParse13 is a test for too many bytes to extract
 */
int DetectBytetestTestParse13(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("9, =, 1, 0", NULL, NULL);
    if (data == NULL) {
        result = 1;
    }

    return result;
}

/**
 * \test DetectBytetestTestParse14 is a test for large string extraction
 */
int DetectBytetestTestParse14(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("23,=,0xffffffffffffffffULL,0,string,oct", NULL, NULL);
    if (data != NULL) {
        if (   (data->op == DETECT_BYTETEST_OP_EQ)
            && (data->nbytes == 23)
            && (data->value == 0xffffffffffffffffULL)
            && (data->offset == 0)
            && (data->flags == DETECT_BYTETEST_STRING)
            && (data->base == DETECT_BYTETEST_BASE_OCT))
        {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test DetectBytetestTestParse15 is a test for too many bytes to extract (string)
 */
int DetectBytetestTestParse15(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("24, =, 0xffffffffffffffffULL, 0, string", NULL, NULL);
    if (data == NULL) {
        result = 1;
    }

    return result;
}

/**
 * \test DetectBytetestTestParse16 is a test for offset too big
 */
int DetectBytetestTestParse16(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4,=,0,0xffffffffffffffffULL", NULL, NULL);
    if (data == NULL) {
        result = 1;
    }

    return result;
}

/**
 * \test Test dce option.
 */
int DetectBytetestTestParse17(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, <, 5, 0, dce", NULL, NULL);
    if (data != NULL) {
        if ( (data->op == DETECT_BYTETEST_OP_LT) &&
             (data->nbytes == 4) &&
             (data->value == 5) &&
             (data->offset == 0) &&
             (data->flags & DETECT_BYTETEST_DCE) ) {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test Test dce option.
 */
int DetectBytetestTestParse18(void)
{
    int result = 0;
    DetectBytetestData *data = NULL;
    data = DetectBytetestParse("4, <, 5, 0", NULL, NULL);
    if (data != NULL) {
        if ( (data->op == DETECT_BYTETEST_OP_LT) &&
             (data->nbytes == 4) &&
             (data->value == 5) &&
             (data->offset == 0) &&
             !(data->flags & DETECT_BYTETEST_DCE) ) {
            result = 1;
        }
        DetectBytetestFree(data);
    }

    return result;
}

/**
 * \test Test dce option.
 */
int DetectBytetestTestParse19(void)
{
    Signature *s = SigAlloc();
    if (s == NULL)
        return 0;

    int result = 1;

    s->alproto = ALPROTO_DCERPC;

    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,dce") == 0);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,string,dce") == -1);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,big,dce") == -1);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,little,dce") == -1);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,hex,dce") == -1);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,oct,dce") == -1);
    result &= (DetectBytetestSetup(NULL, s, "1,=,1,6,dec,dce") == -1);

    SigFree(s);
    return result;
}

/**
 * \test Test dce option.
 */
int DetectBytetestTestParse20(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;
    Signature *s = NULL;
    DetectBytetestData *bd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"Testing bytetest_body\"; "
                               "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                               "dce_stub_data; "
                               "content:\"one\"; distance:0; "
                               "byte_test:1,=,1,6,relative,dce; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }
    s = de_ctx->sig_list;
    if (s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL) {
        result = 0;
        goto end;
    }
    result &= (s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_BYTETEST);
    bd = (DetectBytetestData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    if (!(bd->flags & DETECT_BYTETEST_DCE) &&
        !(bd->flags & DETECT_BYTETEST_RELATIVE) &&
        (bd->flags & DETECT_BYTETEST_STRING) &&
        (bd->flags & DETECT_BYTETEST_BIG) &&
        (bd->flags & DETECT_BYTETEST_LITTLE) &&
        (bd->flags & DETECT_BYTETEST_NEGOP) ) {
        result = 0;
        goto end;
    }

    s->next = SigInit(de_ctx, "alert tcp any any -> any any "
                      "(msg:\"Testing bytetest_body\"; "
                      "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                      "dce_stub_data; "
                      "content:\"one\"; distance:0; "
                      "byte_test:1,=,1,6,relative,dce; sid:1;)");
    if (s->next == NULL) {
        result = 0;
        goto end;
    }
    s = s->next;
    if (s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL) {
        result = 0;
        goto end;
    }
    result &= (s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_BYTETEST);
    bd = (DetectBytetestData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    if (!(bd->flags & DETECT_BYTETEST_DCE) &&
        !(bd->flags & DETECT_BYTETEST_RELATIVE) &&
        (bd->flags & DETECT_BYTETEST_STRING) &&
        (bd->flags & DETECT_BYTETEST_BIG) &&
        (bd->flags & DETECT_BYTETEST_LITTLE) &&
        (bd->flags & DETECT_BYTETEST_NEGOP) ) {
        result = 0;
        goto end;
    }

    s->next = SigInit(de_ctx, "alert tcp any any -> any any "
                      "(msg:\"Testing bytetest_body\"; "
                      "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                      "dce_stub_data; "
                      "content:\"one\"; distance:0; "
                      "byte_test:1,=,1,6,relative; sid:1;)");
    if (s->next == NULL) {
        result = 0;
        goto end;
    }
    s = s->next;
    if (s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL) {
        result = 0;
        goto end;
    }
    result &= (s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_BYTETEST);
    bd = (DetectBytetestData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    if ((bd->flags & DETECT_BYTETEST_DCE) &&
        !(bd->flags & DETECT_BYTETEST_RELATIVE) &&
        (bd->flags & DETECT_BYTETEST_STRING) &&
        (bd->flags & DETECT_BYTETEST_BIG) &&
        (bd->flags & DETECT_BYTETEST_LITTLE) &&
        (bd->flags & DETECT_BYTETEST_NEGOP) ) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Test dce option.
 */
int DetectBytetestTestParse21(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;
    Signature *s = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,string,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,big,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,little,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,hex,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,dec,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,oct,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,string,hex,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,big,string,hex,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,big,string,oct,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,little,string,hex,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

    s = SigInit(de_ctx, "alert tcp any any -> any any "
                "(msg:\"Testing bytetest_body\"; "
                "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                "content:\"one\"; byte_test:1,=,1,6,big,string,dec,dce; sid:1;)");
    if (s != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Test file_data
 */
static int DetectBytetestTestParse22(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    DetectBytetestData *bd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(file_data; byte_test:1,=,1,6,relative; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    s = de_ctx->sig_list;
    if (s->sm_lists_tail[DETECT_SM_LIST_FILEDATA] == NULL) {
        printf("empty server body list: ");
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->type != DETECT_BYTETEST) {
        printf("bytetest not last sm in server body list: ");
        goto end;
    }

    bd = (DetectBytetestData *)s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->ctx;
    if (bd->flags & DETECT_BYTETEST_DCE &&
        bd->flags & DETECT_BYTETEST_RELATIVE &&
        (bd->flags & DETECT_BYTETEST_STRING) &&
        (bd->flags & DETECT_BYTETEST_BIG) &&
        (bd->flags & DETECT_BYTETEST_LITTLE) &&
        (bd->flags & DETECT_BYTETEST_NEGOP) ) {
        printf("wrong flags: ");
        goto end;
    }

    result = 1;
 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test DetectByteTestTestPacket01 is a test to check matches of
 * byte_test and byte_test relative works if the previous keyword is pcre
 * (bug 142)
 */
int DetectByteTestTestPacket01 (void)
{
    int result = 0;
    uint8_t *buf = (uint8_t *)"GET /AllWorkAndNoPlayMakesWillADullBoy HTTP/1.0"
                    "User-Agent: Wget/1.11.4"
                    "Accept: */*"
                    "Host: www.google.com"
                    "Connection: Keep-Alive"
                    "Date: Mon, 04 Jan 2010 17:29:39 GMT";
    uint16_t buflen = strlen((char *)buf);
    Packet *p;
    p = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    if (p == NULL)
        goto end;

    char sig[] = "alert tcp any any -> any any (msg:\"pcre + byte_test + "
    "relative\"; pcre:\"/AllWorkAndNoPlayMakesWillADullBoy/\"; byte_test:1,=,1"
    ",6,relative,string,dec; sid:126; rev:1;)";

    result = UTHPacketMatchSig(p, sig);

    UTHFreePacket(p);
end:
    return result;
}

/**
 * \test DetectByteTestTestPacket02 is a test to check matches of
 * byte_test and byte_test relative works if the previous keyword is byte_jump
 * (bug 158)
 */
int DetectByteTestTestPacket02 (void)
{
    int result = 0;
    uint8_t *buf = (uint8_t *)"GET /AllWorkAndNoPlayMakesWillADullBoy HTTP/1.0"
                    "User-Agent: Wget/1.11.4"
                    "Accept: */*"
                    "Host: www.google.com"
                    "Connection: Keep-Alive"
                    "Date: Mon, 04 Jan 2010 17:29:39 GMT";
    uint16_t buflen = strlen((char *)buf);
    Packet *p;
    p = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    if (p == NULL)
        goto end;

    char sig[] = "alert tcp any any -> any any (msg:\"content + byte_test + "
    "relative\"; byte_jump:1,44,string,dec; byte_test:1,=,0,0,relative,string,"
    "dec; sid:777; rev:1;)";

    result = UTHPacketMatchSig(p, sig);

    UTHFreePacket(p);
end:
    return result;
}

int DetectByteTestTestPacket03(void)
{
    int result = 0;
    uint8_t *buf = NULL;
    uint16_t buflen = 0;
    buf = SCMalloc(4);
    if (unlikely(buf == NULL)) {
        printf("malloc failed\n");
        exit(EXIT_FAILURE);
    }
    memcpy(buf, "boom", 4);
    buflen = 4;

    Packet *p;
    p = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    if (p == NULL)
        goto end;

    char sig[] = "alert tcp any any -> any any (msg:\"content + byte_test\"; "
        "byte_test:1,=,65,214748364; sid:1; rev:1;)";

    result = !UTHPacketMatchSig(p, sig);

    UTHFreePacket(p);

end:
    return result;
}

/** \test Test the byte_test signature matching with operator <= */
int DetectByteTestTestPacket04(void)
{
    int result = 0;
    uint8_t *buf = (uint8_t *)"GET /AllWorkAndNoPlayMakesWillADullBoy HTTP/1.0"
                    "User-Agent: Wget/1.11.4"
                    "Accept: */*"
                    "Host: www.google.com"
                    "Connection: Keep-Alive"
                    "Date: Mon, 04 Jan 2010 17:29:39 GMT";
    uint16_t buflen = strlen((char *)buf);

    Packet *p;
    p = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    if (p == NULL)
        goto end;

    char sig[] = "alert tcp any any -> any any (msg:\"content + byte_test +"
                    "relative\"; content:\"GET \"; depth:4; content:\"HTTP/1.\"; "
    "byte_test:1,<=,0,0,relative,string,dec; sid:124; rev:1;)";

    result = UTHPacketMatchSig(p, sig);

    UTHFreePacket(p);

end:
    return result;
}

/** \test Test the byte_test signature matching with operator >= */
int DetectByteTestTestPacket05(void)
{
    int result = 0;
    uint8_t *buf = (uint8_t *)"GET /AllWorkAndNoPlayMakesWillADullBoy HTTP/1.0"
                    "User-Agent: Wget/1.11.4"
                    "Accept: */*"
                    "Host: www.google.com"
                    "Connection: Keep-Alive"
                    "Date: Mon, 04 Jan 2010 17:29:39 GMT";
    uint16_t buflen = strlen((char *)buf);

    Packet *p;
    p = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    if (p == NULL)
        goto end;

    char sig[] = "alert tcp any any -> any any (msg:\"content + byte_test +"
                    "relative\"; content:\"GET \"; depth:4; content:\"HTTP/1.\"; "
    "byte_test:1,>=,0,0,relative,string,dec; sid:125; rev:1;)";

    result = UTHPacketMatchSig(p, sig);

    UTHFreePacket(p);

end:
    return result;
}

#endif /* UNITTESTS */


/**
 * \brief this function registers unit tests for DetectBytetest
 */
void DetectBytetestRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectBytetestTestParse01", DetectBytetestTestParse01, 1);
    UtRegisterTest("DetectBytetestTestParse02", DetectBytetestTestParse02, 1);
    UtRegisterTest("DetectBytetestTestParse03", DetectBytetestTestParse03, 1);
    UtRegisterTest("DetectBytetestTestParse04", DetectBytetestTestParse04, 1);
    UtRegisterTest("DetectBytetestTestParse05", DetectBytetestTestParse05, 1);
    UtRegisterTest("DetectBytetestTestParse06", DetectBytetestTestParse06, 1);
    UtRegisterTest("DetectBytetestTestParse07", DetectBytetestTestParse07, 1);
    UtRegisterTest("DetectBytetestTestParse08", DetectBytetestTestParse08, 1);
    UtRegisterTest("DetectBytetestTestParse09", DetectBytetestTestParse09, 1);
    UtRegisterTest("DetectBytetestTestParse10", DetectBytetestTestParse10, 1);
    UtRegisterTest("DetectBytetestTestParse11", DetectBytetestTestParse11, 1);
    UtRegisterTest("DetectBytetestTestParse12", DetectBytetestTestParse12, 1);
    UtRegisterTest("DetectBytetestTestParse13", DetectBytetestTestParse13, 1);
    UtRegisterTest("DetectBytetestTestParse14", DetectBytetestTestParse14, 1);
    UtRegisterTest("DetectBytetestTestParse15", DetectBytetestTestParse15, 1);
    UtRegisterTest("DetectBytetestTestParse17", DetectBytetestTestParse17, 1);
    UtRegisterTest("DetectBytetestTestParse18", DetectBytetestTestParse18, 1);
    UtRegisterTest("DetectBytetestTestParse19", DetectBytetestTestParse19, 1);
    UtRegisterTest("DetectBytetestTestParse20", DetectBytetestTestParse20, 1);
    UtRegisterTest("DetectBytetestTestParse21", DetectBytetestTestParse21, 1);
    UtRegisterTest("DetectBytetestTestParse22", DetectBytetestTestParse22, 1);

    UtRegisterTest("DetectByteTestTestPacket01", DetectByteTestTestPacket01, 1);
    UtRegisterTest("DetectByteTestTestPacket02", DetectByteTestTestPacket02, 1);
    UtRegisterTest("DetectByteTestTestPacket03", DetectByteTestTestPacket03, 1);
    UtRegisterTest("DetectByteTestTestPacket04", DetectByteTestTestPacket04, 1);
    UtRegisterTest("DetectByteTestTestPacket05", DetectByteTestTestPacket05, 1);
#endif /* UNITTESTS */
}

