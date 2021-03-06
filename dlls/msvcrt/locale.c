/*
 * msvcrt.dll locale functions
 *
 * Copyright 2000 Jon Griffiths
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

#include "config.h"
#include "wine/port.h"

#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"

#include "msvcrt.h"
#include "mtdll.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msvcrt);

#define MAX_ELEM_LEN 64 /* Max length of country/language/CP string */
#define MAX_LOCALE_LENGTH 256
MSVCRT__locale_t MSVCRT_locale = NULL;
unsigned short *MSVCRT__pctype = NULL;
int MSVCRT___lc_codepage = 0;
int MSVCRT___lc_collate_cp = 0;
LCID MSVCRT___lc_handle[MSVCRT_LC_MAX - MSVCRT_LC_MIN + 1] = { 0 };
int MSVCRT___mb_cur_max = 1;
static unsigned char charmax = CHAR_MAX;

/* MT */
#define LOCK_LOCALE   _mlock(_SETLOCALE_LOCK);
#define UNLOCK_LOCALE _munlock(_SETLOCALE_LOCK);

#define MSVCRT_LEADBYTE  0x8000
#define MSVCRT_C1_DEFINED 0x200

/* Friendly country strings & language names abbreviations. */
static const char * const _country_synonyms[] =
{
    "american", "enu",
    "american english", "enu",
    "american-english", "enu",
    "english-american", "enu",
    "english-us", "enu",
    "english-usa", "enu",
    "us", "enu",
    "usa", "enu",
    "australian", "ena",
    "english-aus", "ena",
    "belgian", "nlb",
    "french-belgian", "frb",
    "canadian", "enc",
    "english-can", "enc",
    "french-canadian", "frc",
    "chinese", "chs",
    "chinese-simplified", "chs",
    "chinese-traditional", "cht",
    "dutch-belgian", "nlb",
    "english-nz", "enz",
    "uk", "eng",
    "english-uk", "eng",
    "french-swiss", "frs",
    "swiss", "des",
    "german-swiss", "des",
    "italian-swiss", "its",
    "german-austrian", "dea",
    "portugese", "ptb",
    "portuguese-brazil", "ptb",
    "spanish-mexican", "esm",
    "norwegian-bokmal", "nor",
    "norwegian-nynorsk", "non",
    "spanish-modern", "esn"
};

/* INTERNAL: Map a synonym to an ISO code */
static void remap_synonym(char *name)
{
  unsigned int i;
  for (i = 0; i < sizeof(_country_synonyms)/sizeof(char*); i += 2 )
  {
    if (!strcasecmp(_country_synonyms[i],name))
    {
      TRACE(":Mapping synonym %s to %s\n",name,_country_synonyms[i+1]);
      strcpy(name, _country_synonyms[i+1]);
      return;
    }
  }
}

/* Note: Flags are weighted in order of matching importance */
#define FOUND_LANGUAGE         0x4
#define FOUND_COUNTRY          0x2
#define FOUND_CODEPAGE         0x1

typedef struct {
  char search_language[MAX_ELEM_LEN];
  char search_country[MAX_ELEM_LEN];
  char search_codepage[MAX_ELEM_LEN];
  char found_codepage[MAX_ELEM_LEN];
  unsigned int match_flags;
  LANGID found_lang_id;
} locale_search_t;

#define CONTINUE_LOOKING TRUE
#define STOP_LOOKING     FALSE

/* INTERNAL: Get and compare locale info with a given string */
static int compare_info(LCID lcid, DWORD flags, char* buff, const char* cmp, BOOL exact)
{
  int len;

  buff[0] = 0;
  GetLocaleInfoA(lcid, flags|LOCALE_NOUSEROVERRIDE, buff, MAX_ELEM_LEN);
  if (!buff[0] || !cmp[0])
    return 0;

  /* Partial matches are only allowed on language/country names */
  len = strlen(cmp);
  if(exact || len<=3)
    return !strcasecmp(cmp, buff);
  else
    return !strncasecmp(cmp, buff, len);
}

static BOOL CALLBACK
find_best_locale_proc(HMODULE hModule, LPCSTR type, LPCSTR name, WORD LangID, LONG_PTR lParam)
{
  locale_search_t *res = (locale_search_t *)lParam;
  const LCID lcid = MAKELCID(LangID, SORT_DEFAULT);
  char buff[MAX_ELEM_LEN];
  unsigned int flags = 0;

  if(PRIMARYLANGID(LangID) == LANG_NEUTRAL)
    return CONTINUE_LOOKING;

  /* Check Language */
  if (compare_info(lcid,LOCALE_SISO639LANGNAME,buff,res->search_language, TRUE) ||
      compare_info(lcid,LOCALE_SABBREVLANGNAME,buff,res->search_language, TRUE) ||
      compare_info(lcid,LOCALE_SENGLANGUAGE,buff,res->search_language, FALSE))
  {
    TRACE(":Found language: %s->%s\n", res->search_language, buff);
    flags |= FOUND_LANGUAGE;
  }
  else if (res->match_flags & FOUND_LANGUAGE)
  {
    return CONTINUE_LOOKING;
  }

  /* Check Country */
  if (compare_info(lcid,LOCALE_SISO3166CTRYNAME,buff,res->search_country, TRUE) ||
      compare_info(lcid,LOCALE_SABBREVCTRYNAME,buff,res->search_country, TRUE) ||
      compare_info(lcid,LOCALE_SENGCOUNTRY,buff,res->search_country, FALSE))
  {
    TRACE("Found country:%s->%s\n", res->search_country, buff);
    flags |= FOUND_COUNTRY;
  }
  else if (res->match_flags & FOUND_COUNTRY)
  {
    return CONTINUE_LOOKING;
  }

  /* Check codepage */
  if (compare_info(lcid,LOCALE_IDEFAULTCODEPAGE,buff,res->search_codepage, TRUE) ||
      (compare_info(lcid,LOCALE_IDEFAULTANSICODEPAGE,buff,res->search_codepage, TRUE)))
  {
    TRACE("Found codepage:%s->%s\n", res->search_codepage, buff);
    flags |= FOUND_CODEPAGE;
    memcpy(res->found_codepage,res->search_codepage,MAX_ELEM_LEN);
  }
  else if (res->match_flags & FOUND_CODEPAGE)
  {
    return CONTINUE_LOOKING;
  }

  if (flags > res->match_flags)
  {
    /* Found a better match than previously */
    res->match_flags = flags;
    res->found_lang_id = LangID;
  }
  if ((flags & (FOUND_LANGUAGE | FOUND_COUNTRY | FOUND_CODEPAGE)) ==
        (FOUND_LANGUAGE | FOUND_COUNTRY | FOUND_CODEPAGE))
  {
    TRACE(":found exact locale match\n");
    return STOP_LOOKING;
  }
  return CONTINUE_LOOKING;
}

extern int atoi(const char *);

/* Internal: Find the LCID for a locale specification */
LCID MSVCRT_locale_to_LCID(const char *locale)
{
    LCID lcid;
    locale_search_t search;
    const char *cp, *region;

    memset(&search, 0, sizeof(locale_search_t));

    cp = strchr(locale, '.');
    region = strchr(locale, '_');

    lstrcpynA(search.search_language, locale, MAX_ELEM_LEN);
    if(region) {
        lstrcpynA(search.search_country, region+1, MAX_ELEM_LEN);
        if(region-locale < MAX_ELEM_LEN)
            search.search_language[region-locale] = '\0';
    } else
        search.search_country[0] = '\0';

    if(cp) {
        lstrcpynA(search.search_codepage, cp+1, MAX_ELEM_LEN);
        if(region && cp-region-1<MAX_ELEM_LEN)
          search.search_country[cp-region-1] = '\0';
        if(cp-locale < MAX_ELEM_LEN)
            search.search_language[cp-locale] = '\0';
    } else
        search.search_codepage[0] = '\0';

    if(!search.search_country[0] && !search.search_codepage[0])
        remap_synonym(search.search_language);

    EnumResourceLanguagesA(GetModuleHandleA("KERNEL32"), (LPSTR)RT_STRING,
            (LPCSTR)LOCALE_ILANGUAGE,find_best_locale_proc,
            (LONG_PTR)&search);

    if (!search.match_flags)
        return -1;

    /* If we were given something that didn't match, fail */
    if (search.search_country[0] && !(search.match_flags & FOUND_COUNTRY))
        return -1;

    lcid =  MAKELCID(search.found_lang_id, SORT_DEFAULT);

    /* Populate partial locale, translating LCID to locale string elements */
    if (!search.found_codepage[0]) {
        /* Even if a codepage is not enumerated for a locale
         * it can be set if valid */
        if (search.search_codepage[0]) {
            if (IsValidCodePage(atoi(search.search_codepage)))
                memcpy(search.found_codepage,search.search_codepage,MAX_ELEM_LEN);
            else {
                /* Special codepage values: OEM & ANSI */
                if (strcasecmp(search.search_codepage,"OCP")) {
                    GetLocaleInfoA(lcid, LOCALE_IDEFAULTCODEPAGE,
                            search.found_codepage, MAX_ELEM_LEN);
                } else if (strcasecmp(search.search_codepage,"ACP")) {
                    GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE,
                            search.found_codepage, MAX_ELEM_LEN);
                } else
                    return -1;

                if (!atoi(search.found_codepage))
                    return -1;
            }
        } else {
            /* Prefer ANSI codepages if present */
            GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE,
                    search.found_codepage, MAX_ELEM_LEN);
            if (!search.found_codepage[0] || !atoi(search.found_codepage))
                GetLocaleInfoA(lcid, LOCALE_IDEFAULTCODEPAGE,
                        search.found_codepage, MAX_ELEM_LEN);
        }
    }

    return lcid;
}

/* INTERNAL: Set lc_handle, lc_id and lc_category in threadlocinfo struct */
static BOOL update_threadlocinfo_category(LCID lcid, MSVCRT__locale_t loc, int category)
{
    char buf[256], *p;
    int len;

    if(GetLocaleInfoA(lcid, LOCALE_ILANGUAGE|LOCALE_NOUSEROVERRIDE, buf, 256)) {
        p = buf;

        loc->locinfo->lc_id[category].wLanguage = 0;
        while(*p) {
            loc->locinfo->lc_id[category].wLanguage *= 16;

            if(*p <= '9')
                loc->locinfo->lc_id[category].wLanguage += *p-'0';
            else
                loc->locinfo->lc_id[category].wLanguage += *p-'a'+10;

            p++;
        }

        loc->locinfo->lc_id[category].wCountry =
            loc->locinfo->lc_id[category].wLanguage;
    }

    if(GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE
                |LOCALE_NOUSEROVERRIDE, buf, 256))
        loc->locinfo->lc_id[category].wCodePage = atoi(buf);

    loc->locinfo->lc_handle[category] = lcid;

    len = 0;
    len += GetLocaleInfoA(lcid, LOCALE_SENGLANGUAGE
            |LOCALE_NOUSEROVERRIDE, buf, 256);
    buf[len-1] = '_';
    len += GetLocaleInfoA(lcid, LOCALE_SENGCOUNTRY
            |LOCALE_NOUSEROVERRIDE, &buf[len], 256-len);
    buf[len-1] = '.';
    len += GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE
            |LOCALE_NOUSEROVERRIDE, &buf[len], 256-len);

    loc->locinfo->lc_category[category].locale = MSVCRT_malloc(len);
    loc->locinfo->lc_category[category].refcount = MSVCRT_malloc(sizeof(int));
    if(!loc->locinfo->lc_category[category].locale
            || !loc->locinfo->lc_category[category].refcount) {
        MSVCRT_free(loc->locinfo->lc_category[category].locale);
        MSVCRT_free(loc->locinfo->lc_category[category].refcount);
        loc->locinfo->lc_category[category].locale = NULL;
        loc->locinfo->lc_category[category].refcount = NULL;
        return TRUE;
    }
    memcpy(loc->locinfo->lc_category[category].locale, buf, len);
    *loc->locinfo->lc_category[category].refcount = 1;

    return FALSE;
}

/* INTERNAL: swap pointers values */
static inline void swap_pointers(void **p1, void **p2) {
    void *hlp;

    hlp = *p1;
    *p1 = *p2;
    *p2 = hlp;
}

/* INTERNAL: returns pthreadlocinfo struct */
MSVCRT_pthreadlocinfo get_locinfo(void) {
    thread_data_t *data = msvcrt_get_thread_data();

    if(!data || !data->have_locale)
        return MSVCRT_locale->locinfo;

    return data->locinfo;
}

/* INTERNAL: returns pthreadlocinfo struct */
MSVCRT_pthreadmbcinfo get_mbcinfo(void) {
    thread_data_t *data = msvcrt_get_thread_data();

    if(!data || !data->have_locale)
        return MSVCRT_locale->mbcinfo;

    return data->mbcinfo;
}

/* INTERNAL: constructs string returned by setlocale */
static inline char* construct_lc_all(MSVCRT_pthreadlocinfo locinfo) {
    static char current_lc_all[MAX_LOCALE_LENGTH];

    int i;

    for(i=MSVCRT_LC_MIN+1; i<MSVCRT_LC_MAX; i++) {
        if(strcmp(locinfo->lc_category[i].locale,
                    locinfo->lc_category[i+1].locale))
            break;
    }

    if(i==MSVCRT_LC_MAX)
        return locinfo->lc_category[MSVCRT_LC_COLLATE].locale;

    sprintf(current_lc_all,
            "LC_COLLATE=%s;LC_CTYPE=%s;LC_MONETARY=%s;LC_NUMERIC=%s;LC_TIME=%s",
            locinfo->lc_category[MSVCRT_LC_COLLATE].locale,
            locinfo->lc_category[MSVCRT_LC_CTYPE].locale,
            locinfo->lc_category[MSVCRT_LC_MONETARY].locale,
            locinfo->lc_category[MSVCRT_LC_NUMERIC].locale,
            locinfo->lc_category[MSVCRT_LC_TIME].locale);

    return current_lc_all;
}


/*********************************************************************
 *		wsetlocale (MSVCRT.@)
 */
MSVCRT_wchar_t* CDECL MSVCRT__wsetlocale(int category, const MSVCRT_wchar_t* locale)
{
  static MSVCRT_wchar_t fake[] = {
    'E','n','g','l','i','s','h','_','U','n','i','t','e','d',' ',
    'S','t','a','t','e','s','.','1','2','5','2',0 };

  FIXME("%d %s\n", category, debugstr_w(locale));

  return fake;
}

/*********************************************************************
 *		_Getdays (MSVCRT.@)
 */
const char* CDECL _Getdays(void)
{
  static const char MSVCRT_days[] = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:"
                            "Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
  /* FIXME: Use locale */
  TRACE("(void) semi-stub\n");
  return MSVCRT_days;
}

/*********************************************************************
 *		_Getmonths (MSVCRT.@)
 */
const char* CDECL _Getmonths(void)
{
  static const char MSVCRT_months[] = ":Jan:January:Feb:February:Mar:March:Apr:"
                "April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:"
                "October:Nov:November:Dec:December";
  /* FIXME: Use locale */
  TRACE("(void) semi-stub\n");
  return MSVCRT_months;
}

/*********************************************************************
 *		_Gettnames (MSVCRT.@)
 */
const char* CDECL _Gettnames(void)
{
  /* FIXME: */
  TRACE("(void) stub\n");
  return "";
}

/*********************************************************************
 *		_Strftime (MSVCRT.@)
 */
const char* CDECL _Strftime(char *out, unsigned int len, const char *fmt,
                            const void *tm, void *foo)
{
  /* FIXME: */
  TRACE("(%p %d %s %p %p) stub\n", out, len, fmt, tm, foo);
  return "";
}

/*********************************************************************
 *		__crtLCMapStringA (MSVCRT.@)
 */
int CDECL __crtLCMapStringA(
  LCID lcid, DWORD mapflags, const char* src, int srclen, char* dst,
  int dstlen, unsigned int codepage, int xflag
) {
  FIXME("(lcid %x, flags %x, %s(%d), %p(%d), %x, %d), partial stub!\n",
        lcid,mapflags,src,srclen,dst,dstlen,codepage,xflag);
  /* FIXME: A bit incorrect. But msvcrt itself just converts its
   * arguments to wide strings and then calls LCMapStringW
   */
  return LCMapStringA(lcid,mapflags,src,srclen,dst,dstlen);
}

/*********************************************************************
 *              __crtLCMapStringW (MSVCRT.@)
 */
int CDECL __crtLCMapStringW(LCID lcid, DWORD mapflags, const MSVCRT_wchar_t *src,
        int srclen, MSVCRT_wchar_t *dst, int dstlen, unsigned int codepage, int xflag)
{
    FIXME("(lcid %x, flags %x, %s(%d), %p(%d), %x, %d), partial stub!\n",
            lcid, mapflags, debugstr_w(src), srclen, dst, dstlen, codepage, xflag);

    return LCMapStringW(lcid, mapflags, src, srclen, dst, dstlen);
}

/*********************************************************************
 *		__crtCompareStringA (MSVCRT.@)
 */
int CDECL __crtCompareStringA( LCID lcid, DWORD flags, const char *src1, int len1,
                               const char *src2, int len2 )
{
    FIXME("(lcid %x, flags %x, %s(%d), %s(%d), partial stub\n",
          lcid, flags, debugstr_a(src1), len1, debugstr_a(src2), len2 );
    /* FIXME: probably not entirely right */
    return CompareStringA( lcid, flags, src1, len1, src2, len2 );
}

/*********************************************************************
 *		__crtCompareStringW (MSVCRT.@)
 */
int CDECL __crtCompareStringW( LCID lcid, DWORD flags, const MSVCRT_wchar_t *src1, int len1,
                               const MSVCRT_wchar_t *src2, int len2 )
{
    FIXME("(lcid %x, flags %x, %s(%d), %s(%d), partial stub\n",
          lcid, flags, debugstr_w(src1), len1, debugstr_w(src2), len2 );
    /* FIXME: probably not entirely right */
    return CompareStringW( lcid, flags, src1, len1, src2, len2 );
}

/*********************************************************************
 *		__crtGetLocaleInfoW (MSVCRT.@)
 */
int CDECL __crtGetLocaleInfoW( LCID lcid, LCTYPE type, MSVCRT_wchar_t *buffer, int len )
{
    FIXME("(lcid %x, type %x, %p(%d), partial stub\n", lcid, type, buffer, len );
    /* FIXME: probably not entirely right */
    return GetLocaleInfoW( lcid, type, buffer, len );
}

/*********************************************************************
 *              btowc(MSVCRT.@)
 */
MSVCRT_wint_t CDECL MSVCRT_btowc(int c)
{
    unsigned char letter = c;
    MSVCRT_wchar_t ret;

    if(!MultiByteToWideChar(get_locinfo()->lc_handle[MSVCRT_LC_CTYPE],
                0, (LPCSTR)&letter, 1, &ret, 1))
        return 0;

    return ret;
}

/*********************************************************************
 *              __crtGetStringTypeW(MSVCRT.@)
 *
 * This function was accepting different number of arguments in older
 * versions of msvcrt.
 */
BOOL CDECL __crtGetStringTypeW(DWORD unk, DWORD type,
        MSVCRT_wchar_t *buffer, int len, WORD *out)
{
    FIXME("(unk %x, type %x, wstr %p(%d), %p) partial stub\n",
            unk, type, buffer, len, out);

    return GetStringTypeW(type, buffer, len, out);
}

/*********************************************************************
 *		localeconv (MSVCRT.@)
 */
struct MSVCRT_lconv * CDECL MSVCRT_localeconv(void)
{
    return get_locinfo()->lconv;
}

/*********************************************************************
 *		__lconv_init (MSVCRT.@)
 */
int CDECL __lconv_init(void)
{
    /* this is used to make chars unsigned */
    charmax = 255;
    return 0;
}

/*********************************************************************
 *      ___lc_handle_func (MSVCRT.@)
 */
LCID* CDECL ___lc_handle_func(void)
{
    return MSVCRT___lc_handle;
}

/*********************************************************************
 *      ___lc_codepage_func (MSVCRT.@)
 */
int CDECL ___lc_codepage_func(void)
{
    return MSVCRT___lc_codepage;
}

/*********************************************************************
 *      ___lc_collate_cp_func (MSVCRT.@)
 */
int CDECL ___lc_collate_cp_func(void)
{
    return get_locinfo()->lc_collate_cp;
}

/* INTERNAL: frees MSVCRT_pthreadlocinfo struct */
void free_locinfo(MSVCRT_pthreadlocinfo locinfo)
{
    int i;

    if(!locinfo)
        return;

    for(i=MSVCRT_LC_MIN+1; i<=MSVCRT_LC_MAX; i++) {
        MSVCRT_free(locinfo->lc_category[i].locale);
        MSVCRT_free(locinfo->lc_category[i].refcount);
    }

    if(locinfo->lconv) {
        MSVCRT_free(locinfo->lconv->decimal_point);
        MSVCRT_free(locinfo->lconv->thousands_sep);
        MSVCRT_free(locinfo->lconv->grouping);
        MSVCRT_free(locinfo->lconv->int_curr_symbol);
        MSVCRT_free(locinfo->lconv->currency_symbol);
        MSVCRT_free(locinfo->lconv->mon_decimal_point);
        MSVCRT_free(locinfo->lconv->mon_thousands_sep);
        MSVCRT_free(locinfo->lconv->mon_grouping);
        MSVCRT_free(locinfo->lconv->positive_sign);
        MSVCRT_free(locinfo->lconv->negative_sign);
    }
    MSVCRT_free(locinfo->lconv_intl_refcount);
    MSVCRT_free(locinfo->lconv_num_refcount);
    MSVCRT_free(locinfo->lconv_mon_refcount);
    MSVCRT_free(locinfo->lconv);

    MSVCRT_free(locinfo->ctype1_refcount);
    MSVCRT_free(locinfo->ctype1);

    MSVCRT_free(locinfo->pclmap);
    MSVCRT_free(locinfo->pcumap);

    MSVCRT_free(locinfo);
}

/* INTERNAL: frees MSVCRT_pthreadmbcinfo struct */
void free_mbcinfo(MSVCRT_pthreadmbcinfo mbcinfo)
{
    if(!mbcinfo)
        return;

    MSVCRT_free(mbcinfo);
}

/* _free_locale - not exported in native msvcrt */
void CDECL MSVCRT__free_locale(MSVCRT__locale_t locale)
{
    if (!locale)
        return;

    free_locinfo(locale->locinfo);
    free_mbcinfo(locale->mbcinfo);
    MSVCRT_free(locale);
}

/* _create_locale - not exported in native msvcrt */
MSVCRT__locale_t CDECL MSVCRT__create_locale(int category, const char *locale)
{
    static const char collate[] = "COLLATE=";
    static const char ctype[] = "CTYPE=";
    static const char monetary[] = "MONETARY=";
    static const char numeric[] = "NUMERIC=";
    static const char time[] = "TIME=";

    MSVCRT__locale_t loc;
    LCID lcid[6] = { 0 };
    char buf[256];
    int i;

    TRACE("(%d %s)\n", category, locale);

    if(category<MSVCRT_LC_MIN || category>MSVCRT_LC_MAX || !locale)
        return NULL;

    if(locale[0]=='C' && !locale[1])
        lcid[0] = CP_ACP;
    else if(!locale[0])
        lcid[0] = GetSystemDefaultLCID();
    else if (locale[0] == 'L' && locale[1] == 'C' && locale[2] == '_') {
        const char *p;

        while(1) {
            locale += 3; /* LC_ */
            if(!memcmp(locale, collate, sizeof(collate)-1)) {
                i = MSVCRT_LC_COLLATE;
                locale += sizeof(collate)-1;
            } else if(!memcmp(locale, ctype, sizeof(ctype)-1)) {
                i = MSVCRT_LC_CTYPE;
                locale += sizeof(ctype)-1;
            } else if(!memcmp(locale, monetary, sizeof(monetary)-1)) {
                i = MSVCRT_LC_MONETARY;
                locale += sizeof(monetary)-1;
            } else if(!memcmp(locale, numeric, sizeof(numeric)-1)) {
                i = MSVCRT_LC_NUMERIC;
                locale += sizeof(numeric)-1;
            } else if(!memcmp(locale, time, sizeof(time)-1)) {
                i = MSVCRT_LC_TIME;
                locale += sizeof(time)-1;
            } else
                return NULL;

            p = strchr(locale, ';');
            if(locale[0]=='C' && (locale[1]==';' || locale[1]=='\0'))
                lcid[i] = 0;
            else if(p) {
                memcpy(buf, locale, p-locale);
                buf[p-locale] = '\0';
                lcid[i] = MSVCRT_locale_to_LCID(buf);
            } else
                lcid[i] = MSVCRT_locale_to_LCID(locale);

            if(lcid[i] == -1)
                return NULL;

            if(!p || *(p+1)!='L' || *(p+2)!='C' || *(p+3)!='_')
                break;

            locale = p+1;
        }
    } else {
        lcid[0] = MSVCRT_locale_to_LCID(locale);
        if(lcid[0] == -1)
            return NULL;
    }

    for(i=1; i<6; i++) {
        if(!lcid[i])
            lcid[i] = lcid[0];
    }

    loc = MSVCRT_malloc(sizeof(MSVCRT__locale_tstruct));
    if(!loc)
        return NULL;

    loc->locinfo = MSVCRT_malloc(sizeof(MSVCRT_threadlocinfo));
    if(!loc->locinfo) {
        MSVCRT_free(loc);
        return NULL;
    }

    loc->mbcinfo = MSVCRT_malloc(sizeof(MSVCRT_threadmbcinfo));
    if(!loc->mbcinfo) {
        MSVCRT_free(loc->locinfo);
        MSVCRT_free(loc);
        return NULL;
    }

    memset(loc->locinfo, 0, sizeof(MSVCRT_threadlocinfo));

    loc->locinfo->lconv = MSVCRT_malloc(sizeof(struct MSVCRT_lconv));
    if(!loc->locinfo->lconv) {
        MSVCRT__free_locale(loc);
        return NULL;
    }
    memset(loc->locinfo->lconv, 0, sizeof(struct MSVCRT_lconv));

    loc->locinfo->pclmap = MSVCRT_malloc(sizeof(char[256]));
    loc->locinfo->pcumap = MSVCRT_malloc(sizeof(char[256]));
    if(!loc->locinfo->pclmap || !loc->locinfo->pcumap) {
        MSVCRT__free_locale(loc);
        return NULL;
    }

    loc->locinfo->refcount = 1;

    if(lcid[MSVCRT_LC_COLLATE] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_COLLATE)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_COLLATE], loc, MSVCRT_LC_COLLATE)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }
    } else
        loc->locinfo->lc_category[MSVCRT_LC_COLLATE].locale = MSVCRT__strdup("C");

    if(lcid[MSVCRT_LC_CTYPE] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_CTYPE)) {
        CPINFO cp;
        int j;

        if(update_threadlocinfo_category(lcid[MSVCRT_LC_CTYPE], loc, MSVCRT_LC_CTYPE)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        loc->locinfo->lc_codepage = loc->locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage;
        loc->locinfo->lc_collate_cp = loc->locinfo->lc_codepage;
        loc->locinfo->lc_clike = 1;
        if(!GetCPInfo(loc->locinfo->lc_codepage, &cp)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }
        loc->locinfo->mb_cur_max = cp.MaxCharSize;

        loc->locinfo->ctype1_refcount = MSVCRT_malloc(sizeof(int));
        loc->locinfo->ctype1 = MSVCRT_malloc(sizeof(short[257]));
        if(!loc->locinfo->ctype1_refcount || !loc->locinfo->ctype1) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        *loc->locinfo->ctype1_refcount = 1;
        loc->locinfo->ctype1[0] = 0;
        loc->locinfo->pctype = loc->locinfo->ctype1+1;

        buf[1] = buf[2] = '\0';
        for(i=1; i<257; i++) {
            buf[0] = i-1;

            /* builtin GetStringTypeA doesn't set output to 0 on invalid input */
            loc->locinfo->ctype1[i] = 0;

            GetStringTypeA(lcid[MSVCRT_LC_CTYPE], CT_CTYPE1, buf,
                    1, loc->locinfo->ctype1+i);
        }

        for(i=0; cp.LeadByte[i+1]!=0; i+=2)
            for(j=cp.LeadByte[i]; j<=cp.LeadByte[i+1]; j++)
                loc->locinfo->ctype1[j+1] |= MSVCRT__LEADBYTE;
    } else {
        loc->locinfo->lc_clike = 1;
        loc->locinfo->mb_cur_max = 1;
        loc->locinfo->pctype = MSVCRT__ctype+1;
        loc->locinfo->lc_category[MSVCRT_LC_CTYPE].locale = MSVCRT__strdup("C");
    }

    for(i=0; i<256; i++) {
        if(loc->locinfo->pctype[i] & MSVCRT__LEADBYTE)
            buf[i] = ' ';
        else
            buf[i] = i;

    }

    if(lcid[MSVCRT_LC_CTYPE]) {
        LCMapStringA(lcid[MSVCRT_LC_CTYPE], LCMAP_LOWERCASE, buf, 256,
                (char*)loc->locinfo->pclmap, 256);
        LCMapStringA(lcid[MSVCRT_LC_CTYPE], LCMAP_UPPERCASE, buf, 256,
                (char*)loc->locinfo->pcumap, 256);
    } else {
        for(i=0; i<256; i++) {
            loc->locinfo->pclmap[i] = (i>='A' && i<='Z' ? i-'A'+'a' : i);
            loc->locinfo->pcumap[i] = (i>='a' && i<='z' ? i-'a'+'A' : i);
        }
    }

    _setmbcp_l(loc->locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage, lcid[MSVCRT_LC_CTYPE], loc->mbcinfo);

    if(lcid[MSVCRT_LC_MONETARY] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_MONETARY)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_MONETARY], loc, MSVCRT_LC_MONETARY)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        loc->locinfo->lconv_intl_refcount = MSVCRT_malloc(sizeof(int));
        loc->locinfo->lconv_mon_refcount = MSVCRT_malloc(sizeof(int));
        if(!loc->locinfo->lconv_intl_refcount || !loc->locinfo->lconv_mon_refcount) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        *loc->locinfo->lconv_intl_refcount = 1;
        *loc->locinfo->lconv_mon_refcount = 1;

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SINTLSYMBOL
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->int_curr_symbol = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->int_curr_symbol, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SCURRENCY
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->currency_symbol = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->currency_symbol, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONDECIMALSEP
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->mon_decimal_point = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->mon_decimal_point, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONTHOUSANDSEP
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->mon_thousands_sep = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->mon_thousands_sep, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONGROUPING
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i>1)
            i = i/2 + (buf[i-2]=='0'?0:1);
        if(i && (loc->locinfo->lconv->mon_grouping = MSVCRT_malloc(i))) {
            for(i=0; buf[i+1]==';'; i+=2)
                loc->locinfo->lconv->mon_grouping[i/2] = buf[i]-'0';
            loc->locinfo->lconv->mon_grouping[i/2] = buf[i]-'0';
            if(buf[i] != '0')
                loc->locinfo->lconv->mon_grouping[i/2+1] = 127;
        } else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SPOSITIVESIGN
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->positive_sign = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->positive_sign, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SNEGATIVESIGN
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->negative_sign = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->negative_sign, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IINTLCURRDIGITS
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->int_frac_digits = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_ICURRDIGITS
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->frac_digits = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSYMPRECEDES
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->p_cs_precedes = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSEPBYSPACE
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->p_sep_by_space = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSYMPRECEDES
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->n_cs_precedes = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSEPBYSPACE
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->n_sep_by_space = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSIGNPOSN
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->p_sign_posn = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSIGNPOSN
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            loc->locinfo->lconv->n_sign_posn = atoi(buf);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }
    } else {
        loc->locinfo->lconv->int_curr_symbol = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->currency_symbol = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->mon_decimal_point = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->mon_thousands_sep = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->mon_grouping = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->positive_sign = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->negative_sign = MSVCRT_malloc(sizeof(char));

        if(!loc->locinfo->lconv->int_curr_symbol || !loc->locinfo->lconv->currency_symbol
                || !loc->locinfo->lconv->mon_decimal_point || !loc->locinfo->lconv->mon_thousands_sep
                || !loc->locinfo->lconv->mon_grouping || !loc->locinfo->lconv->positive_sign
                || !loc->locinfo->lconv->negative_sign) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        loc->locinfo->lconv->int_curr_symbol[0] = '\0';
        loc->locinfo->lconv->currency_symbol[0] = '\0';
        loc->locinfo->lconv->mon_decimal_point[0] = '\0';
        loc->locinfo->lconv->mon_thousands_sep[0] = '\0';
        loc->locinfo->lconv->mon_grouping[0] = '\0';
        loc->locinfo->lconv->positive_sign[0] = '\0';
        loc->locinfo->lconv->negative_sign[0] = '\0';
        loc->locinfo->lconv->int_frac_digits = 127;
        loc->locinfo->lconv->frac_digits = 127;
        loc->locinfo->lconv->p_cs_precedes = 127;
        loc->locinfo->lconv->p_sep_by_space = 127;
        loc->locinfo->lconv->n_cs_precedes = 127;
        loc->locinfo->lconv->n_sep_by_space = 127;
        loc->locinfo->lconv->p_sign_posn = 127;
        loc->locinfo->lconv->n_sign_posn = 127;

        loc->locinfo->lc_category[MSVCRT_LC_MONETARY].locale = MSVCRT__strdup("C");
    }

    if(lcid[MSVCRT_LC_NUMERIC] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_NUMERIC)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_NUMERIC], loc, MSVCRT_LC_NUMERIC)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        if(!loc->locinfo->lconv_intl_refcount)
            loc->locinfo->lconv_intl_refcount = MSVCRT_malloc(sizeof(int));
        loc->locinfo->lconv_num_refcount = MSVCRT_malloc(sizeof(int));
        if(!loc->locinfo->lconv_intl_refcount || !loc->locinfo->lconv_num_refcount) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        *loc->locinfo->lconv_intl_refcount = 1;
        *loc->locinfo->lconv_num_refcount = 1;

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_SDECIMAL
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->decimal_point = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->decimal_point, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_STHOUSAND
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (loc->locinfo->lconv->thousands_sep = MSVCRT_malloc(i)))
            memcpy(loc->locinfo->lconv->thousands_sep, buf, i);
        else {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_SGROUPING
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i>1)
            i = i/2 + (buf[i-2]=='0'?0:1);
        if(i && (loc->locinfo->lconv->grouping = MSVCRT_malloc(i))) {
            for(i=0; buf[i+1]==';'; i+=2)
                loc->locinfo->lconv->grouping[i/2] = buf[i]-'0';
            loc->locinfo->lconv->grouping[i/2] = buf[i]-'0';
            if(buf[i] != '0')
                loc->locinfo->lconv->grouping[i/2+1] = 127;
        } else {
            MSVCRT__free_locale(loc);
            return NULL;
        }
    } else {
        loc->locinfo->lconv->decimal_point = MSVCRT_malloc(sizeof(char[2]));
        loc->locinfo->lconv->thousands_sep = MSVCRT_malloc(sizeof(char));
        loc->locinfo->lconv->grouping = MSVCRT_malloc(sizeof(char));
        if(!loc->locinfo->lconv->decimal_point || !loc->locinfo->lconv->thousands_sep
                || !loc->locinfo->lconv->grouping) {
            MSVCRT__free_locale(loc);
            return NULL;
        }

        loc->locinfo->lconv->decimal_point[0] = '.';
        loc->locinfo->lconv->decimal_point[1] = '\0';
        loc->locinfo->lconv->thousands_sep[0] = '\0';
        loc->locinfo->lconv->grouping[0] = '\0';

        loc->locinfo->lc_category[MSVCRT_LC_NUMERIC].locale = MSVCRT__strdup("C");
    }

    if(lcid[MSVCRT_LC_TIME] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_TIME)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_TIME], loc, MSVCRT_LC_TIME)) {
            MSVCRT__free_locale(loc);
            return NULL;
        }
    } else
        loc->locinfo->lc_category[MSVCRT_LC_TIME].locale = MSVCRT__strdup("C");

    return loc;
}

/*********************************************************************
 *             setlocale (MSVCRT.@)
 */
char* CDECL MSVCRT_setlocale(int category, const char* locale)
{
    MSVCRT__locale_t loc;
    MSVCRT_pthreadlocinfo locinfo = get_locinfo();

    if(category<MSVCRT_LC_MIN || category>MSVCRT_LC_MAX)
        return NULL;

    if(!locale) {
        if(category == MSVCRT_LC_ALL)
            return construct_lc_all(locinfo);

        return locinfo->lc_category[category].locale;
    }

    loc = MSVCRT__create_locale(category, locale);
    if(!loc) {
        WARN("%d %s failed\n", category, locale);
        return NULL;
    }

    LOCK_LOCALE;

    switch(category) {
        case MSVCRT_LC_ALL:
        case MSVCRT_LC_COLLATE:
            locinfo->lc_handle[MSVCRT_LC_COLLATE] =
                loc->locinfo->lc_handle[MSVCRT_LC_COLLATE];
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_COLLATE].locale,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_COLLATE].locale);
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_COLLATE].refcount,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_COLLATE].refcount);

            if(category != MSVCRT_LC_ALL)
                break;
            /* fall through */
        case MSVCRT_LC_CTYPE:
            locinfo->lc_handle[MSVCRT_LC_CTYPE] =
                loc->locinfo->lc_handle[MSVCRT_LC_CTYPE];
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_CTYPE].locale,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_CTYPE].locale);
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_CTYPE].refcount,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_CTYPE].refcount);

            locinfo->lc_codepage = loc->locinfo->lc_codepage;
            locinfo->lc_collate_cp = loc->locinfo->lc_collate_cp;
            locinfo->lc_clike = loc->locinfo->lc_clike;
            locinfo->mb_cur_max = loc->locinfo->mb_cur_max;

            swap_pointers((void**)&locinfo->ctype1_refcount,
                    (void**)&loc->locinfo->ctype1_refcount);
            swap_pointers((void**)&locinfo->ctype1, (void**)&loc->locinfo->ctype1);
            swap_pointers((void**)&locinfo->pctype, (void**)&loc->locinfo->pctype);
            swap_pointers((void**)&locinfo->pclmap, (void**)&loc->locinfo->pclmap);
            swap_pointers((void**)&locinfo->pcumap, (void**)&loc->locinfo->pcumap);

            if(category != MSVCRT_LC_ALL)
                break;
            /* fall through */
        case MSVCRT_LC_MONETARY:
            locinfo->lc_handle[MSVCRT_LC_MONETARY] =
                loc->locinfo->lc_handle[MSVCRT_LC_MONETARY];
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_MONETARY].locale,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_MONETARY].locale);
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_MONETARY].refcount,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_MONETARY].refcount);

            swap_pointers((void**)&locinfo->lconv->int_curr_symbol,
                    (void**)&loc->locinfo->lconv->int_curr_symbol);
            swap_pointers((void**)&locinfo->lconv->currency_symbol,
                    (void**)&loc->locinfo->lconv->currency_symbol);
            swap_pointers((void**)&locinfo->lconv->mon_decimal_point,
                    (void**)&loc->locinfo->lconv->mon_decimal_point);
            swap_pointers((void**)&locinfo->lconv->mon_thousands_sep,
                    (void**)&loc->locinfo->lconv->mon_thousands_sep);
            swap_pointers((void**)&locinfo->lconv->mon_grouping,
                    (void**)&loc->locinfo->lconv->mon_grouping);
            swap_pointers((void**)&locinfo->lconv->positive_sign,
                    (void**)&loc->locinfo->lconv->positive_sign);
            swap_pointers((void**)&locinfo->lconv->negative_sign,
                    (void**)&loc->locinfo->lconv->negative_sign);
            locinfo->lconv->int_frac_digits = loc->locinfo->lconv->int_frac_digits;
            locinfo->lconv->frac_digits = loc->locinfo->lconv->frac_digits;
            locinfo->lconv->p_cs_precedes = loc->locinfo->lconv->p_cs_precedes;
            locinfo->lconv->p_sep_by_space = loc->locinfo->lconv->p_sep_by_space;
            locinfo->lconv->n_cs_precedes = loc->locinfo->lconv->n_cs_precedes;
            locinfo->lconv->n_sep_by_space = loc->locinfo->lconv->n_sep_by_space;
            locinfo->lconv->p_sign_posn = loc->locinfo->lconv->p_sign_posn;
            locinfo->lconv->n_sign_posn = loc->locinfo->lconv->n_sign_posn;

            if(category != MSVCRT_LC_ALL)
                break;
            /* fall through */
        case MSVCRT_LC_NUMERIC:
            locinfo->lc_handle[MSVCRT_LC_NUMERIC] =
                loc->locinfo->lc_handle[MSVCRT_LC_NUMERIC];
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_NUMERIC].locale,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_NUMERIC].locale);
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_NUMERIC].refcount,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_NUMERIC].refcount);

            swap_pointers((void**)&locinfo->lconv->decimal_point,
                    (void**)&loc->locinfo->lconv->decimal_point);
            swap_pointers((void**)&locinfo->lconv->thousands_sep,
                    (void**)&loc->locinfo->lconv->thousands_sep);
            swap_pointers((void**)&locinfo->lconv->grouping,
                    (void**)&loc->locinfo->lconv->grouping);

            if(category != MSVCRT_LC_ALL)
                break;
            /* fall through */
        case MSVCRT_LC_TIME:
            locinfo->lc_handle[MSVCRT_LC_TIME] =
                loc->locinfo->lc_handle[MSVCRT_LC_TIME];
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_TIME].locale,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_TIME].locale);
            swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_TIME].refcount,
                    (void**)&loc->locinfo->lc_category[MSVCRT_LC_TIME].refcount);

            if(category != MSVCRT_LC_ALL)
                break;
    }

    MSVCRT__free_locale(loc);
    UNLOCK_LOCALE;

    if(locinfo == MSVCRT_locale->locinfo) {
        int i;

        MSVCRT___lc_codepage = locinfo->lc_codepage;
        MSVCRT___lc_collate_cp = locinfo->lc_collate_cp;
        MSVCRT___mb_cur_max = locinfo->mb_cur_max;
        MSVCRT__pctype = locinfo->pctype;
        for(i=MSVCRT_LC_MIN; i<=MSVCRT_LC_MAX; i++)
            MSVCRT___lc_handle[i] = MSVCRT_locale->locinfo->lc_handle[i];
    }

    if(category == MSVCRT_LC_ALL)
        return construct_lc_all(locinfo);

    return locinfo->lc_category[category].locale;
}

/* _configthreadlocale - not exported in native msvcrt */
int CDECL _configthreadlocale(int type)
{
    thread_data_t *data = msvcrt_get_thread_data();
    MSVCRT__locale_t locale;
    int ret;

    if(!data)
        return -1;

    ret = (data->have_locale ? MSVCRT__ENABLE_PER_THREAD_LOCALE : MSVCRT__DISABLE_PER_THREAD_LOCALE);

    if(type == MSVCRT__ENABLE_PER_THREAD_LOCALE) {
        if(!data->have_locale) {
            /* Copy current global locale */
            locale = MSVCRT__create_locale(MSVCRT_LC_ALL, MSVCRT_setlocale(MSVCRT_LC_ALL, NULL));
            if(!locale)
                return -1;

            data->locinfo = locale->locinfo;
            data->mbcinfo = locale->mbcinfo;
            data->have_locale = TRUE;
            MSVCRT_free(locale);
        }

        return ret;
    }

    if(type == MSVCRT__DISABLE_PER_THREAD_LOCALE) {
        if(data->have_locale) {
            free_locinfo(data->locinfo);
            free_mbcinfo(data->mbcinfo);
            data->locinfo = MSVCRT_locale->locinfo;
            data->mbcinfo = MSVCRT_locale->mbcinfo;
            data->have_locale = FALSE;
        }

        return ret;
    }

    if(!type)
        return ret;

    return -1;
}

BOOL msvcrt_init_locale(void)
{
    int i;

    LOCK_LOCALE;
    MSVCRT_locale = MSVCRT__create_locale(0, "C");
    UNLOCK_LOCALE;
    if(!MSVCRT_locale)
        return FALSE;

    MSVCRT___lc_codepage = MSVCRT_locale->locinfo->lc_codepage;
    MSVCRT___lc_collate_cp = MSVCRT_locale->locinfo->lc_collate_cp;
    MSVCRT___mb_cur_max = MSVCRT_locale->locinfo->mb_cur_max;
    MSVCRT__pctype = MSVCRT_locale->locinfo->pctype;
    for(i=MSVCRT_LC_MIN; i<=MSVCRT_LC_MAX; i++)
        MSVCRT___lc_handle[i] = MSVCRT_locale->locinfo->lc_handle[i];
    _setmbcp(_MB_CP_ANSI);
    return TRUE;
}
