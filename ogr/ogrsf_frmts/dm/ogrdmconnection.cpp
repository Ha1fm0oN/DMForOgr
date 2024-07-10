/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRDGNLayer class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam (warmerdam@pobox.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_dm.h"
#include "cpl_conv.h"

/************************************************************************/
/*                          OGRGetDMConnection()                        */
/************************************************************************/

OGRDMConn* OGRGetDMConnection(const char* pszUserid,
                              const char* pszPassword,
                              const char* pszDatabase)
{
    OGRDMConn *poConnection;

    poConnection = new OGRDMConn();
    if (poConnection->EstablishConn(pszUserid, pszPassword, pszDatabase))
        return poConnection;
    else
    {
        delete poConnection;
        return nullptr;
    }
}

/************************************************************************/
/*                          OGRDMSession()                              */
/************************************************************************/

OGRDMConn::OGRDMConn()
{
    hEnv = nullptr;
    hRtn = 0;
    hCon = nullptr;
    pszUserid = nullptr;
    pszPassword = nullptr;
    pszDatabase = nullptr;
}

/************************************************************************/
/*                          ~OGRDMSession()                             */
/************************************************************************/

OGRDMConn::~OGRDMConn()
{
    DPIRETURN rt;
    rt = dpi_commit(hCon);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "failed to commit!");
    }
    dpi_logout(hCon);
    dpi_free_con(hCon);
    dpi_free_env(hEnv);
    CPLFree(pszUserid);
    CPLFree(pszPassword);
    CPLFree(pszDatabase);
}

/************************************************************************/
/*                          EstablishSession()                          */
/************************************************************************/
int OGRDMConn::EstablishConn(const char* pszUseridIn,
                             const char* pszPasswordIn,
                             const char* pszDatabaseIn)
{
    DPIRETURN rt;

    rt = dpi_alloc_env(&hEnv);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to alloc environment handle!");
        return FALSE;
    }

    rt = dpi_alloc_con(hEnv, &hCon);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to alloc connection handle");
        return FALSE;
    }

    rt = dpi_login(hCon, (sdbyte *)pszDatabaseIn, (sdbyte *)pszUseridIn,
                   (sdbyte *)pszPasswordIn);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "failed to login");
        return FALSE;
    }
    rt = dpi_set_con_attr(hCon, DSQL_ATTR_AUTOCOMMIT, DSQL_AUTOCOMMIT_OFF, 0);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "failed to set con_attr");
        return FALSE;
    }

    pszUserid = CPLStrdup(pszUseridIn);
    pszPassword = CPLStrdup(pszPasswordIn);
    pszDatabase = CPLStrdup(pszDatabaseIn);

    return TRUE;
}
