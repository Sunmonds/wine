/*
 *    ParseError implementation
 *
 * Copyright 2005 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#define COBJMACROS

#include "config.h"

#include <stdarg.h>
#ifdef HAVE_LIBXML2
# include <libxml/parser.h>
# include <libxml/xmlerror.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "ole2.h"
#include "msxml6.h"

#include "msxml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msxml);

typedef struct
{
    DispatchEx dispex;
    IXMLDOMParseError IXMLDOMParseError_iface;
    LONG ref;
    LONG code, line, linepos, filepos;
    BSTR url, reason, srcText;
} parse_error_t;

static inline parse_error_t *impl_from_IXMLDOMParseError( IXMLDOMParseError *iface )
{
    return CONTAINING_RECORD(iface, parse_error_t, IXMLDOMParseError_iface);
}

static HRESULT WINAPI parseError_QueryInterface(
    IXMLDOMParseError *iface,
    REFIID riid,
    void** ppvObject )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppvObject);

    if ( IsEqualGUID( riid, &IID_IUnknown ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IXMLDOMParseError ) )
    {
        *ppvObject = iface;
    }
    else if (dispex_query_interface(&This->dispex, riid, ppvObject))
    {
        return *ppvObject ? S_OK : E_NOINTERFACE;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    IXMLDOMParseError_AddRef( iface );

    return S_OK;
}

static ULONG WINAPI parseError_AddRef(
    IXMLDOMParseError *iface )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    ULONG ref = InterlockedIncrement( &This->ref );
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI parseError_Release(
    IXMLDOMParseError *iface )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE("(%p)->(%d)\n", This, ref);
    if ( ref == 0 )
    {
        SysFreeString(This->url);
        SysFreeString(This->reason);
        SysFreeString(This->srcText);
        release_dispex(&This->dispex);
        heap_free( This );
    }

    return ref;
}

static HRESULT WINAPI parseError_GetTypeInfoCount(
    IXMLDOMParseError *iface,
    UINT* pctinfo )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI parseError_GetTypeInfo(
    IXMLDOMParseError *iface,
    UINT iTInfo,
    LCID lcid,
    ITypeInfo** ppTInfo )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface,
        iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI parseError_GetIDsOfNames(
    IXMLDOMParseError *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface,
        riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI parseError_Invoke(
    IXMLDOMParseError *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface,
        dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI parseError_get_errorCode(
    IXMLDOMParseError *iface,
    LONG *code )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    TRACE("(%p)->(%p)\n", This, code);

    *code = This->code;

    if(This->code == 0)
        return S_FALSE;

    return S_OK;
}

static HRESULT WINAPI parseError_get_url(
    IXMLDOMParseError *iface,
    BSTR *url )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    FIXME("(%p)->(%p)\n", This, url);
    return E_NOTIMPL;
}

static HRESULT WINAPI parseError_get_reason(
    IXMLDOMParseError *iface,
    BSTR *reason )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    TRACE("(%p)->(%p)\n", This, reason);
    
    if(!This->reason)
    {
        *reason = NULL;
        return S_FALSE;
    }
    *reason = SysAllocString(This->reason);
    return S_OK;
}

static HRESULT WINAPI parseError_get_srcText(
    IXMLDOMParseError *iface,
    BSTR *srcText )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );

    TRACE("(%p)->(%p)\n", This, srcText);

    if (!srcText) return E_INVALIDARG;

    *srcText = SysAllocString(This->srcText);

    return S_OK;
}

static HRESULT WINAPI parseError_get_line(
    IXMLDOMParseError *iface,
    LONG *line )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );

    TRACE("(%p)->(%p): stub\n", This, line);

    if (!line) return E_INVALIDARG;

    *line = This->line;
    return S_OK;
}

static HRESULT WINAPI parseError_get_linepos(
    IXMLDOMParseError *iface,
    LONG *linepos )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );

    TRACE("(%p)->(%p)\n", This, linepos);

    if (!linepos) return E_INVALIDARG;

    *linepos = This->linepos;
    return S_OK;
}

static HRESULT WINAPI parseError_get_filepos(
    IXMLDOMParseError *iface,
    LONG *filepos )
{
    parse_error_t *This = impl_from_IXMLDOMParseError( iface );
    FIXME("(%p)->(%p)\n", This, filepos);
    return E_NOTIMPL;
}

static const struct IXMLDOMParseErrorVtbl parseError_vtbl =
{
    parseError_QueryInterface,
    parseError_AddRef,
    parseError_Release,
    parseError_GetTypeInfoCount,
    parseError_GetTypeInfo,
    parseError_GetIDsOfNames,
    parseError_Invoke,
    parseError_get_errorCode,
    parseError_get_url,
    parseError_get_reason,
    parseError_get_srcText,
    parseError_get_line,
    parseError_get_linepos,
    parseError_get_filepos
};

static const tid_t parseError_iface_tids[] = {
    IXMLDOMParseError_tid,
    0
};

static dispex_static_data_t parseError_dispex = {
    NULL,
    IXMLDOMParseError_tid,
    NULL,
    parseError_iface_tids
};

IXMLDOMParseError *create_parseError( LONG code, BSTR url, BSTR reason, BSTR srcText,
                                      LONG line, LONG linepos, LONG filepos )
{
    parse_error_t *This;

    This = heap_alloc( sizeof(*This) );
    if ( !This )
        return NULL;

    This->IXMLDOMParseError_iface.lpVtbl = &parseError_vtbl;
    This->ref = 1;

    This->code = code;
    This->url = url;
    This->reason = reason;
    This->srcText = srcText;
    This->line = line;
    This->linepos = linepos;
    This->filepos = filepos;

    init_dispex(&This->dispex, (IUnknown*)&This->IXMLDOMParseError_iface, &parseError_dispex);

    return &This->IXMLDOMParseError_iface;
}
