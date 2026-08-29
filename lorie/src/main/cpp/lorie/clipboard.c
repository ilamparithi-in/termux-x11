/* Copyright 2016-2019 Pierre Ossman for Cendio AB
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#pragma clang diagnostic ignored "-Wunknown-pragmas"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <android/log.h>
#include <X11/Xatom.h>
#include <windowstr.h>
#include <selection.h>
#include <propertyst.h>
#include <xacestr.h>

#include "lorie.h"

/* utility functions for text conversion */

static inline void lorieConvertLF(const char* src, char *dst, size_t bytes) {
    size_t i = 0, j = 0;
    for (; i < bytes; i++)
        if (src[i] != '\r')
            dst[j++] = src[i];
}

static inline void lorieLatin1ToUTF8(unsigned char* out, const unsigned char* in) {
    while (*in)
        if (*in < 128)
            *out++ = *in++;
        else
            *out++ = 0xc2 + (*in > 0xbf), *out++ = (*in++ & 0x3f) + 0x80;
}

static inline int lorieCheckUTF8(const unsigned char *utf, size_t size) {
    int ix;
    unsigned char c;

    for (ix = 0; (c = utf[ix]) && ix < size;) {
        if (c & 0x80) {
            if ((utf[ix + 1] & 0xc0) != 0x80)
                return 0;
            if ((c & 0xe0) == 0xe0) {
                if ((utf[ix + 2] & 0xc0) != 0x80)
                    return 0;
                if ((c & 0xf0) == 0xf0) {
                    if ((c & 0xf8) != 0xf0 || (utf[ix + 3] & 0xc0) != 0x80)
                        return 0;
                    ix += 4;
                    /* 4-byte code */
                } else
                    /* 3-byte code */
                    ix += 3;
            } else
                /* 2-byte code */
                ix += 2;
        } else
            /* 1-byte code */
            ix++;
    }
    return 1;
}

static size_t lorieUtf8ToUCS4(const char* src, size_t max, unsigned* dst) {
    size_t count, consumed;

    *dst = 0xfffd;

    if (max == 0)
        return 0;

    consumed = 1;

    if ((*src & 0x80) == 0) {
        *dst = *src;
        count = 0;
    } else if ((*src & 0xe0) == 0xc0) {
        *dst = *src & 0x1f;
        count = 1;
    } else if ((*src & 0xf0) == 0xe0) {
        *dst = *src & 0x0f;
        count = 2;
    } else if ((*src & 0xf8) == 0xf0) {
        *dst = *src & 0x07;
        count = 3;
    } else {
        // Invalid sequence, consume all continuation characters
        src++;
        max--;
        while ((max-- > 0) && ((*src++ & 0xc0) == 0x80))
            consumed++;
        return consumed;
    }

    src++;
    max--;

    while (count--) {
        consumed++;

        // Invalid or truncated sequence?
        if ((max == 0) || ((*src & 0xc0) != 0x80)) {
            *dst = 0xfffd;
            return consumed;
        }

        *dst <<= 6;
        *dst |= *src & 0x3f;

        src++;
        max--;
    }

    // UTF-16 surrogate code point?
    if ((*dst >= 0xd800) && (*dst < 0xe000))
        *dst = 0xfffd;

    return consumed;
}

static const char *lorieUtf8ToLatin1(const char *src) {
    size_t sz;

    const char* in;
    size_t in_len;

    // Compute output size
    sz = 0;
    in = src;
    in_len = -1;
    while ((in_len > 0) && (*in != '\0')) {
        size_t len;
        unsigned ucs;

        len = lorieUtf8ToUCS4(in, in_len, &ucs);
        in += len;
        in_len -= len;
        sz++;
    }

    // Reserve space
    unsigned char out[sz + 1];
    memset(out, 0, sz + 1);
    size_t position = 0;

    // And convert
    in = src;
    in_len = 4.294967295E9;
    while ((in_len > 0) && (*in != '\0')) {
        size_t len;
        unsigned ucs;

        len = lorieUtf8ToUCS4(in, in_len, &ucs);
        in += len;
        in_len -= len;

        if (ucs > 0xff)
            out[position++] = '?';
        else
            out[position++] = (unsigned char)ucs;
    }

    return strdup((const char*) out);
}

/* end utility functions */

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)
extern ScreenPtr pScreenPtr;

static int (*origProcSendEvent)(ClientPtr) = NULL;
static int (*origProcConvertSelection)(ClientPtr) = NULL;
static Atom xaTIMESTAMP = 0, xaTEXT = 0, xaCLIPBOARD = 0, xaTARGETS = 0, xaSTRING = 0, xaUTF8_STRING = 0;
static Atom xaPNG = 0, xaPNG_UPPER = 0, xaJPEG = 0, xaIMAGE_JPG = 0, xaJPEG_UPPER = 0, xaBMP = 0;
static Bool clipboardEnabled = FALSE;

typedef struct {
    uint8_t type;
    char *text_data;
    size_t text_len;
    uint8_t *image_data;
    size_t image_len;
} StagedClipboard;

static StagedClipboard g_staged_clip = {LORIE_CLIPBOARD_NONE, NULL, 0, NULL, 0};

void lorieEnableClipboardSync(Bool enable) {
    clipboardEnabled = enable;
}

/* functions related to clipboard receiving from X11 apps */

static void lorieSelectionRequest(Atom selection, Atom target) {
    Selection *pSel;

    if (clipboardEnabled && dixLookupSelection(&pSel, selection, serverClient, DixGetAttrAccess) == Success) {
        xEvent event = {0};
        event.u.u.type = SelectionRequest;
        event.u.selectionRequest.owner = pSel->window;
        event.u.selectionRequest.time = currentTime.milliseconds;
        event.u.selectionRequest.requestor = pScreenPtr->root->drawable.id;
        event.u.selectionRequest.selection = selection;
        event.u.selectionRequest.target = target;
        event.u.selectionRequest.property = target;
        WriteEventsToClient(pSel->client, 1, &event);
    }
}

static Bool lorieHasAtom(Atom atom, const Atom list[], size_t size) {
    for (size_t i = 0; i < size; i++)
        if (list[i] == atom)
            return TRUE;

    return FALSE;
}

static void lorieHandleSelection(Atom target) {
    PropertyPtr prop;
    if (target != xaTARGETS && target != xaSTRING && target != xaUTF8_STRING &&
        target != xaPNG && target != xaPNG_UPPER && target != xaJPEG && target != xaIMAGE_JPG &&
        target != xaJPEG_UPPER && target != xaBMP)
        return;

    if (dixLookupProperty(&prop, pScreenPtr->root, target, serverClient, DixReadAccess) != Success)
        return;

    log(DEBUG, "Selection notification for CLIPBOARD (target %s, type %s)\n", NameForAtom(target), NameForAtom(prop->type));

    if (target == xaTARGETS && prop->type == XA_ATOM && prop->format == 32) {
        if (lorieHasAtom(xaPNG, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaPNG);
        else if (lorieHasAtom(xaPNG_UPPER, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaPNG_UPPER);
        else if (lorieHasAtom(xaJPEG, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaJPEG);
        else if (lorieHasAtom(xaIMAGE_JPG, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaIMAGE_JPG);
        else if (lorieHasAtom(xaJPEG_UPPER, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaJPEG_UPPER);
        else if (lorieHasAtom(xaBMP, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaBMP);
        else if (lorieHasAtom(xaUTF8_STRING, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaUTF8_STRING);
        else if (lorieHasAtom(xaSTRING, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaSTRING);
    } else if (target == xaPNG || target == xaPNG_UPPER || target == xaJPEG ||
               target == xaIMAGE_JPG || target == xaJPEG_UPPER || target == xaBMP) {
        log(DEBUG, "Sending X11 clipboard image to Android (%zu bytes)\n", (size_t) prop->size);
        lorieSendClipboardData((const char*) prop->data, prop->size, LORIE_CLIPBOARD_IMAGE_PNG);
    } else if (target == xaSTRING && prop->type == xaSTRING && prop->format == 8) {
        char filtered[prop->size + 1], utf8[(prop->size + 1) * 2];
        memset(filtered, 0, sizeof(filtered));
        memset(utf8, 0, sizeof(utf8));

        lorieConvertLF(prop->data,  filtered, prop->size);
        lorieLatin1ToUTF8((unsigned char*) utf8, (unsigned char*) filtered);
        log(DEBUG, "Sending X11 clipboard text to Android (%zu bytes)\n", strlen(utf8));
        lorieSendClipboardData(utf8, strlen(utf8), LORIE_CLIPBOARD_TEXT);
    } else if (target == xaUTF8_STRING && prop->type == xaUTF8_STRING && prop->format == 8) {
        char filtered[prop->size + 1];

        if (!lorieCheckUTF8(prop->data, prop->size)) {
            dprintf(2, "Invalid UTF-8 sequence in clipboard\n");
            return;
        }

        memset(filtered, 0, prop->size + 1);
        lorieConvertLF(prop->data, filtered, prop->size);

        log(DEBUG, "Sending X11 clipboard text to Android (%zu bytes)\n", strlen(filtered));
        lorieSendClipboardData(filtered, strlen(filtered), LORIE_CLIPBOARD_TEXT);
    }
}

static int lorieProcSendEvent(ClientPtr client) {
    REQUEST(xSendEventReq);
    REQUEST_SIZE_MATCH(xSendEventReq);
    __typeof__(stuff->event.u.selectionNotify)* e = &stuff->event.u.selectionNotify;

    if (clipboardEnabled && e->requestor == pScreenPtr->root->drawable.id &&
        stuff->event.u.u.type == SelectionNotify && e->selection == xaCLIPBOARD && e->target == e->property)
        lorieHandleSelection(e->target);

    return origProcSendEvent(client);
}

static void lorieSelectionCallback(__unused CallbackListPtr *callbacks, __unused void * data, void * args) {
    SelectionInfoRec *info = (SelectionInfoRec *) args;

    if (clipboardEnabled && info->selection->selection == xaCLIPBOARD && info->kind == SelectionSetOwner) {
        if (info->selection->client != serverClient) {
            // An external X11 client (e.g. Xournal++) took ownership of the selection.
            // Invalidate our staged clipboard so we do not interfere with internal vector copy/paste.
            lorieClearStagedClipboard();
            // Query targets from the X11 owner to mirror to Android clipboard.
            lorieSelectionRequest(xaCLIPBOARD, xaTARGETS);
        }
    }
}

/* end functions related to clipboard receiving from X11 apps */

/* functions related to pre-staged clipboard serving */

static int lorieOwnSelection(Atom selection) {
    Selection *pSel;
    int rc;
    SelectionInfoRec info;

    rc = dixLookupSelection(&pSel, selection, serverClient, DixSetAttrAccess);
    if (rc == Success) {
        if (pSel->client && (pSel->client != serverClient)) {
            xEvent event = {
                .u.selectionClear.time = currentTime.milliseconds,
                .u.selectionClear.window = pSel->window,
                .u.selectionClear.atom = pSel->selection
            };
            event.u.u.type = SelectionClear;
            WriteEventsToClient(pSel->client, 1, &event);
        }
    } else if (rc == BadMatch) {
        pSel = dixAllocateObjectWithPrivates(Selection, PRIVATE_SELECTION);
        if (!pSel)
            return BadAlloc;

        pSel->selection = selection;

        rc = XaceHookSelectionAccess(serverClient, &pSel, DixCreateAccess | DixSetAttrAccess);
        if (rc != Success) {
            free(pSel);
            return rc;
        }

        pSel->next = CurrentSelections;
        CurrentSelections = pSel;
    } else {
        return rc;
    }

    pSel->lastTimeChanged = currentTime;
    pSel->window = pScreenPtr->root->drawable.id;
    pSel->pWin = pScreenPtr->root;
    pSel->client = serverClient;

    log(DEBUG, "Grabbed %s selection (pre-staged push model)", NameForAtom(selection));

    info.selection = pSel;
    info.client = serverClient;
    info.kind = SelectionSetOwner;
    CallCallbacks(&SelectionCallback, &info);

    return Success;
}

void lorieClearStagedClipboard(void) {
    if (g_staged_clip.text_data) {
        free(g_staged_clip.text_data);
        g_staged_clip.text_data = NULL;
    }
    if (g_staged_clip.image_data) {
        free(g_staged_clip.image_data);
        g_staged_clip.image_data = NULL;
    }
    g_staged_clip.text_len = 0;
    g_staged_clip.image_len = 0;
    g_staged_clip.type = LORIE_CLIPBOARD_NONE;
}

void lorieStageClipboard(uint8_t type, const char* data, size_t len) {
    lorieClearStagedClipboard();

    if (type == LORIE_CLIPBOARD_IMAGE_PNG && data && len > 0) {
        g_staged_clip.image_data = (uint8_t*) malloc(len);
        if (g_staged_clip.image_data) {
            memcpy(g_staged_clip.image_data, data, len);
            g_staged_clip.image_len = len;
            g_staged_clip.type = LORIE_CLIPBOARD_IMAGE_PNG;
            log(DEBUG, "Pre-staged image clipboard (%zu bytes PNG)", len);
        }
    } else if (type == LORIE_CLIPBOARD_TEXT && data && len > 0) {
        g_staged_clip.text_data = (char*) malloc(len + 1);
        if (g_staged_clip.text_data) {
            memcpy(g_staged_clip.text_data, data, len);
            g_staged_clip.text_data[len] = '\0';
            g_staged_clip.text_len = len;
            g_staged_clip.type = LORIE_CLIPBOARD_TEXT;
            log(DEBUG, "Pre-staged text clipboard (%zu bytes UTF-8)", len);
        }
    }

    if (g_staged_clip.type != LORIE_CLIPBOARD_NONE) {
        lorieOwnSelection(xaCLIPBOARD);
    }
}

void lorieHandleClipboardAnnounce(void) {
    // No-op or clear if unstage requested
}

static int lorieConvertSelection(ClientPtr client, Atom selection, Atom target, Atom property, Window requestor, CARD32 time) {
    Selection *pSel;
    WindowPtr pWin;
    int rc;

    Atom realProperty;
    xEvent event;

    rc = dixLookupSelection(&pSel, selection, client, DixGetAttrAccess);
    if (rc != Success)
        return rc;

    rc = dixLookupWindow(&pWin, requestor, client, DixSetAttrAccess);
    if (rc != Success)
        return rc;

    realProperty = (property != None) ? property : target;

    if (target == xaTARGETS) {
        if (g_staged_clip.type == LORIE_CLIPBOARD_IMAGE_PNG && g_staged_clip.image_len > 0) {
            Atom targets[] = { xaTARGETS, xaTIMESTAMP,
                               xaPNG, xaPNG_UPPER, xaJPEG, xaIMAGE_JPG, xaJPEG_UPPER, xaBMP };
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_ATOM, 32, PropModeReplace,
                                         sizeof(targets)/sizeof(targets[0]),
                                         targets, TRUE);
        } else if (g_staged_clip.type == LORIE_CLIPBOARD_TEXT && g_staged_clip.text_len > 0) {
            Atom targets[] = { xaTARGETS, xaTIMESTAMP,
                               xaUTF8_STRING, xaSTRING, xaTEXT };
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_ATOM, 32, PropModeReplace,
                                         sizeof(targets)/sizeof(targets[0]),
                                         targets, TRUE);
        } else {
            Atom targets[] = { xaTARGETS, xaTIMESTAMP };
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_ATOM, 32, PropModeReplace,
                                         sizeof(targets)/sizeof(targets[0]),
                                         targets, TRUE);
        }
        if (rc != Success)
            return rc;
    } else if (target == xaTIMESTAMP) {
        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_INTEGER, 32, PropModeReplace, 1,
                                     &pSel->lastTimeChanged.milliseconds,
                                     TRUE);
        if (rc != Success)
            return rc;
    } else if (target == xaPNG || target == xaPNG_UPPER || target == xaJPEG ||
               target == xaIMAGE_JPG || target == xaJPEG_UPPER || target == xaBMP) {
        if (g_staged_clip.type == LORIE_CLIPBOARD_IMAGE_PNG && g_staged_clip.image_data && g_staged_clip.image_len > 0) {
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         target, 8, PropModeReplace,
                                         g_staged_clip.image_len, g_staged_clip.image_data, TRUE);
            if (rc != Success)
                return rc;
            log(DEBUG, "Instantly served pre-staged image to X11 client (%zu bytes)", g_staged_clip.image_len);
        } else {
            return BadMatch;
        }
    } else if (target == xaUTF8_STRING) {
        if (g_staged_clip.type == LORIE_CLIPBOARD_TEXT && g_staged_clip.text_data && g_staged_clip.text_len > 0) {
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         xaUTF8_STRING, 8, PropModeReplace,
                                         g_staged_clip.text_len, g_staged_clip.text_data, TRUE);
            if (rc != Success)
                return rc;
            log(DEBUG, "Instantly served pre-staged UTF-8 text to X11 client (%zu bytes)", g_staged_clip.text_len);
        } else {
            return BadMatch;
        }
    } else if (target == xaSTRING || target == xaTEXT) {
        if (g_staged_clip.type == LORIE_CLIPBOARD_TEXT && g_staged_clip.text_data && g_staged_clip.text_len > 0) {
            const char* latin1 = lorieUtf8ToLatin1(g_staged_clip.text_data);
            if (latin1 == NULL)
                return BadAlloc;

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_STRING, 8, PropModeReplace,
                                         strlen(latin1), latin1, TRUE);
            free((void*) latin1);
            if (rc != Success)
                return rc;
            log(DEBUG, "Instantly served pre-staged Latin-1 text to X11 client");
        } else {
            return BadMatch;
        }
    } else {
        return BadMatch;
    }

    memset(&event, 0, sizeof(xEvent));
    event.u.u.type = SelectionNotify;
    event.u.selectionNotify.time = time;
    event.u.selectionNotify.requestor = requestor;
    event.u.selectionNotify.selection = selection;
    event.u.selectionNotify.target = target;
    event.u.selectionNotify.property = realProperty;
    WriteEventsToClient(client, 1, &event);
    return Success;
}

static int lorieProcConvertSelection(ClientPtr client) {
    Bool paramsOkay;
    WindowPtr pWin;
    Selection *pSel;
    int rc;

    REQUEST(xConvertSelectionReq);
    REQUEST_SIZE_MATCH(xConvertSelectionReq);

    rc = dixLookupWindow(&pWin, stuff->requestor, client, DixSetAttrAccess);
    if (rc != Success)
        return rc;

    paramsOkay = ValidAtom(stuff->selection) && ValidAtom(stuff->target);
    paramsOkay &= (stuff->property == None) || ValidAtom(stuff->property);
    if (!paramsOkay) {
        client->errorValue = stuff->property;
        return BadAtom;
    }

    /* Do we own this selection? */
    rc = dixLookupSelection(&pSel, stuff->selection, client, DixReadAccess);
    if (rc == Success && pSel->client == serverClient && pSel->window == pScreenPtr->root->drawable.id) {
        rc = lorieConvertSelection(client, stuff->selection,
                                   stuff->target, stuff->property,
                                   stuff->requestor, stuff->time);
        if (rc != Success) {
            xEvent event;
            memset(&event, 0, sizeof(xEvent));
            event.u.u.type = SelectionNotify;
            event.u.selectionNotify.time = stuff->time;
            event.u.selectionNotify.requestor = stuff->requestor;
            event.u.selectionNotify.selection = stuff->selection;
            event.u.selectionNotify.target = stuff->target;
            event.u.selectionNotify.property = None;
            WriteEventsToClient(client, 1, &event);
        }

        return Success;
    }

    return origProcConvertSelection(client);
}

void lorieInitClipboard(void) {
#define ATOM(name) xa##name = MakeAtom(#name, strlen(#name), TRUE)
    ATOM(TIMESTAMP); ATOM(TEXT); ATOM(CLIPBOARD); ATOM(TARGETS); ATOM(STRING); ATOM(UTF8_STRING);
    xaPNG = MakeAtom("image/png", 9, TRUE);
    xaPNG_UPPER = MakeAtom("PNG", 3, TRUE);
    xaJPEG = MakeAtom("image/jpeg", 10, TRUE);
    xaIMAGE_JPG = MakeAtom("image/jpg", 9, TRUE);
    xaJPEG_UPPER = MakeAtom("JPEG", 4, TRUE);
    xaBMP = MakeAtom("image/bmp", 9, TRUE);

    if (!origProcConvertSelection) {
        origProcConvertSelection = ProcVector[X_ConvertSelection];
        ProcVector[X_ConvertSelection] = lorieProcConvertSelection;
    }

    if (!origProcSendEvent) {
        origProcSendEvent = ProcVector[X_SendEvent];
        ProcVector[X_SendEvent] = lorieProcSendEvent;
    }

    if (!AddCallback(&SelectionCallback, lorieSelectionCallback, NULL))
        FatalError("Adding SelectionCallback failed\n");
}
