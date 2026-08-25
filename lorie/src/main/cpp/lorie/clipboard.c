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
static Atom xaPNG = 0, xaPNG_UPPER = 0, xaJPEG = 0, xaIMAGE_JPG = 0, xaJPEG_UPPER = 0, xaBMP = 0, xaHTML = 0, xaHTML_UTF8 = 0;
static Bool clipboardEnabled = FALSE;

static struct {
    char* text;
    size_t textSize;
    char* image;
    size_t imageSize;
    char* html;
    size_t htmlSize;
    uint8_t fetchedMask; // bit 0 = text, bit 1 = image, bit 2 = html
} clipCache = {NULL, 0, NULL, 0, NULL, 0, 0};

struct LorieDataTarget {
    ClientPtr client;
    Atom selection;
    Atom target;
    Atom property;
    Window requestor;
    CARD32 time;
    struct LorieDataTarget* next;
} *lorieDataTargetHead;

void lorieEnableClipboardSync(Bool enable) {
    clipboardEnabled = enable;
}

/* functions related to clipboard receiving */

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
        target != xaJPEG_UPPER && target != xaBMP && target != xaHTML && target != xaHTML_UTF8)
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
        else if (lorieHasAtom(xaHTML, (const Atom*)prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaHTML);
    } else if (target == xaPNG || target == xaPNG_UPPER || target == xaJPEG ||
               target == xaIMAGE_JPG || target == xaJPEG_UPPER || target == xaBMP) {
        log(DEBUG, "Sending clipboard image to Android (%zu bytes)\n", (size_t) prop->size);
        lorieSendClipboardData((const char*) prop->data, prop->size, LORIE_CLIPBOARD_IMAGE_PNG);
    } else if (target == xaHTML || target == xaHTML_UTF8) {
        log(DEBUG, "Sending clipboard HTML to Android (%zu bytes)\n", (size_t) prop->size);
        lorieSendClipboardData((const char*) prop->data, prop->size, LORIE_CLIPBOARD_HTML);
    } else if (target == xaSTRING && prop->type == xaSTRING && prop->format == 8) {
        char filtered[prop->size + 1], utf8[(prop->size + 1) * 2];
        memset(filtered, 0, sizeof(filtered));
        memset(utf8, 0, sizeof(utf8));

        lorieConvertLF(prop->data,  filtered, prop->size);
        lorieLatin1ToUTF8((unsigned char*) utf8, (unsigned char*) filtered);
        log(DEBUG, "Sending clipboard to clients (%zu bytes)\n", strlen(utf8));
        lorieSendClipboardData(utf8, strlen(utf8), LORIE_CLIPBOARD_TEXT);
    } else if (target == xaUTF8_STRING && prop->type == xaUTF8_STRING && prop->format == 8) {
        char filtered[prop->size + 1];

        if (!lorieCheckUTF8(prop->data, prop->size)) {
            dprintf(2, "Invalid UTF-8 sequence in clipboard\n");
            return;
        }

        memset(filtered, 0, prop->size + 1);
        lorieConvertLF(prop->data, filtered, prop->size);

        log(DEBUG, "Sending clipboard to clients (%zu bytes)\n", strlen(filtered));
        lorieSendClipboardData(filtered, strlen(filtered), LORIE_CLIPBOARD_TEXT);
    }
}

static int lorieProcSendEvent(ClientPtr client) {
    REQUEST(xSendEventReq)
    REQUEST_SIZE_MATCH(xSendEventReq);
    __typeof__(stuff->event.u.selectionNotify)* e = &stuff->event.u.selectionNotify;

    if (clipboardEnabled && e->requestor == pScreenPtr->root->drawable.id &&
        stuff->event.u.u.type == SelectionNotify && e->selection == xaCLIPBOARD && e->target == e->property)
        lorieHandleSelection(e->target);

    return origProcSendEvent(client);
}

static void lorieSelectionCallback(__unused CallbackListPtr *callbacks, __unused void * data, void * args) {
    SelectionInfoRec *info = (SelectionInfoRec *) args;

    if (clipboardEnabled && info->selection->selection == xaCLIPBOARD && info->kind == SelectionSetOwner && info->selection->client != serverClient)
        lorieSelectionRequest(xaCLIPBOARD, xaTARGETS);
}

/* end functions related to clipboard receiving */

/* functions related to clipboard announcing and sending */

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
        Atom targets[] = { xaTARGETS, xaTIMESTAMP,
                           xaUTF8_STRING, xaSTRING, xaTEXT,
                           xaHTML, xaHTML_UTF8,
                           xaPNG, xaPNG_UPPER, xaJPEG, xaIMAGE_JPG, xaJPEG_UPPER, xaBMP };

        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_ATOM, 32, PropModeReplace,
                                     sizeof(targets)/sizeof(targets[0]),
                                     targets, TRUE);
        if (rc != Success)
            return rc;
    } else if (target == xaTIMESTAMP) {
        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_INTEGER, 32, PropModeReplace, 1,
                                     &pSel->lastTimeChanged.milliseconds,
                                     TRUE);
        if (rc != Success)
            return rc;
    } else {
        uint8_t reqType = LORIE_CLIPBOARD_TEXT;
        if (target == xaPNG || target == xaPNG_UPPER || target == xaJPEG ||
            target == xaIMAGE_JPG || target == xaJPEG_UPPER || target == xaBMP)
            reqType = LORIE_CLIPBOARD_IMAGE_PNG;
        else if (target == xaHTML || target == xaHTML_UTF8)
            reqType = LORIE_CLIPBOARD_HTML;
        else if (target != xaUTF8_STRING && target != xaSTRING && target != xaTEXT)
            return BadMatch;

        if (!(clipCache.fetchedMask & (1 << reqType))) {
            struct LorieDataTarget* ldt = calloc(1, sizeof(struct LorieDataTarget));
            if (ldt == NULL)
                return BadAlloc;

            ldt->client = client;
            ldt->selection = selection;
            ldt->target = target;
            ldt->property = property;
            ldt->requestor = requestor;
            ldt->time = time;

            ldt->next = lorieDataTargetHead;
            lorieDataTargetHead = ldt;

            log(DEBUG, "Requesting clipboard data from Android (targetType=%d)", (int) reqType);
            lorieRequestClipboard(reqType);

            return Success;
        }

        if (reqType == LORIE_CLIPBOARD_IMAGE_PNG) {
            if (clipCache.image && clipCache.imageSize > 0) {
                rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                             target, 8, PropModeReplace,
                                             clipCache.imageSize, clipCache.image, TRUE);
                if (rc != Success)
                    return rc;
            } else {
                return BadMatch;
            }
        } else if (reqType == LORIE_CLIPBOARD_HTML) {
            if (clipCache.html && clipCache.htmlSize > 0) {
                rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                             target, 8, PropModeReplace,
                                             clipCache.htmlSize, clipCache.html, TRUE);
                if (rc != Success)
                    return rc;
            } else {
                return BadMatch;
            }
        } else { // TEXT
            if (clipCache.text && clipCache.textSize > 0) {
                if (target == xaUTF8_STRING) {
                    rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                                 xaUTF8_STRING, 8, PropModeReplace,
                                                 clipCache.textSize, clipCache.text, TRUE);
                    if (rc != Success)
                        return rc;
                } else {
                    const char* latin1 = lorieUtf8ToLatin1(clipCache.text);
                    if (latin1 == NULL)
                        return BadAlloc;

                    rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                                 XA_STRING, 8, PropModeReplace,
                                                 strlen(latin1), latin1, TRUE);
                    free((void*) latin1);
                    if (rc != Success)
                        return rc;
                }
            } else {
                return BadMatch;
            }
        }
    }

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

    REQUEST(xConvertSelectionReq)
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
    }
    else
        return rc;

    pSel->lastTimeChanged = currentTime;
    pSel->window = pScreenPtr->root->drawable.id;
    pSel->pWin = pScreenPtr->root;
    pSel->client = serverClient;

    log(DEBUG, "Grabbed %s selection", NameForAtom(selection));

    info.selection = pSel;
    info.client = serverClient;
    info.kind = SelectionSetOwner;
    CallCallbacks(&SelectionCallback, &info);

    return Success;
}

void lorieHandleClipboardAnnounce(void) {
    free(clipCache.text);
    free(clipCache.image);
    free(clipCache.html);
    memset(&clipCache, 0, sizeof(clipCache));

    int rc;

    log(DEBUG, "Remote clipboard announced, grabbing local ownership");

    rc = lorieOwnSelection(xaCLIPBOARD);
    if (rc != Success)
        log(ERROR, "Could not set CLIPBOARD selection");
}

void lorieHandleClipboardData(uint8_t mimeType, const char* data, size_t len) {
    log(DEBUG, "Got remote clipboard data (%zu bytes, mimeType=%u), sending to X11 clients", len, (unsigned int) mimeType);

    clipCache.fetchedMask |= (1 << mimeType);

    if (mimeType == LORIE_CLIPBOARD_IMAGE_PNG) {
        free(clipCache.image);
        clipCache.image = NULL;
        clipCache.imageSize = len;
        if (data && len > 0) {
            clipCache.image = (char*) malloc(len);
            if (clipCache.image) memcpy(clipCache.image, data, len);
        }
    } else if (mimeType == LORIE_CLIPBOARD_HTML) {
        free(clipCache.html);
        clipCache.html = NULL;
        clipCache.htmlSize = len;
        if (data && len > 0) {
            clipCache.html = (char*) malloc(len + 1);
            if (clipCache.html) {
                memcpy(clipCache.html, data, len);
                clipCache.html[len] = 0;
            }
        }
    } else { // TEXT
        free(clipCache.text);
        clipCache.text = NULL;
        clipCache.textSize = len;
        if (data && len > 0) {
            clipCache.text = (char*) malloc(len + 1);
            if (clipCache.text) {
                memcpy(clipCache.text, data, len);
                clipCache.text[len] = 0;
            }
        }
    }

    struct LorieDataTarget** curr = &lorieDataTargetHead;
    while (*curr != NULL) {
        struct LorieDataTarget* ldt = *curr;
        uint8_t reqType = LORIE_CLIPBOARD_TEXT;
        if (ldt->target == xaPNG || ldt->target == xaJPEG)
            reqType = LORIE_CLIPBOARD_IMAGE_PNG;
        else if (ldt->target == xaHTML || ldt->target == xaHTML_UTF8)
            reqType = LORIE_CLIPBOARD_HTML;

        if (reqType == mimeType) {
            *curr = ldt->next;
            int rc = lorieConvertSelection(ldt->client, ldt->selection, ldt->target, ldt->property, ldt->requestor, ldt->time);
            if (rc != Success) {
                xEvent event = {0};
                event.u.u.type = SelectionNotify;
                event.u.selectionNotify.time = ldt->time;
                event.u.selectionNotify.requestor = ldt->requestor;
                event.u.selectionNotify.selection = ldt->selection;
                event.u.selectionNotify.target = ldt->target;
                event.u.selectionNotify.property = None;
                WriteEventsToClient(ldt->client, 1, &event);
            }
            free(ldt);
        } else {
            curr = &(*curr)->next;
        }
    }
}

/* end functions related to clipboard announcing and sending */

void lorieInitClipboard(void) {
#define ATOM(name) xa##name = MakeAtom(#name, strlen(#name), TRUE)
    ATOM(TIMESTAMP); ATOM(TEXT); ATOM(CLIPBOARD); ATOM(TARGETS); ATOM(STRING); ATOM(UTF8_STRING);
    xaPNG = MakeAtom("image/png", 9, TRUE);
    xaPNG_UPPER = MakeAtom("PNG", 3, TRUE);
    xaJPEG = MakeAtom("image/jpeg", 10, TRUE);
    xaIMAGE_JPG = MakeAtom("image/jpg", 9, TRUE);
    xaJPEG_UPPER = MakeAtom("JPEG", 4, TRUE);
    xaBMP = MakeAtom("image/bmp", 9, TRUE);
    xaHTML = MakeAtom("text/html", 9, TRUE);
    xaHTML_UTF8 = MakeAtom("text/html;charset=utf-8", 23, TRUE);

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
