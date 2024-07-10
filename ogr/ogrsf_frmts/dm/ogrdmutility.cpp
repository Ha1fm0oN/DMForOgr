#include "ogr_dm.h"
#include "cpl_conv.h"

CPL_CVSID("$Id$")

#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char *strToupper(char *str)
{
    char *str1 = str;
    while (*str)
    {
        *str = (char)toupper((unsigned char)*str);
        str++;
    }
    return str1;
}

/************************************************************************/
/*                       OGRDMCommonLayerGetType()                      */
/************************************************************************/

CPLString OGRDMCommonLayerGetType(OGRFieldDefn &oField,
                                  bool bPreservePrecision,
                                  bool bApproxOK)
{
    const char *pszFieldType = "";

    /* -------------------------------------------------------------------- */
    /*      Work out the DM type.                                           */
    /* -------------------------------------------------------------------- */
    if (oField.GetType() == OFTInteger)
    {
        if (oField.GetSubType() == OFSTBoolean)
            pszFieldType = "BIT";
        else if (oField.GetSubType() == OFSTInt16)
            pszFieldType = "SMALLINT";
        else if (oField.GetWidth() > 0 && bPreservePrecision)
            pszFieldType = CPLSPrintf("NUMERIC(%d,0)", oField.GetWidth());
        else
            pszFieldType = "INTEGER";
    }
    else if (oField.GetType() == OFTInteger64)
    {
        if (oField.GetWidth() > 0 && bPreservePrecision)
            pszFieldType = CPLSPrintf("NUMERIC(%d,0)", oField.GetWidth());
        else
            pszFieldType = "BIGINT";
    }
    else if (oField.GetType() == OFTReal)
    {
        if (oField.GetSubType() == OFSTFloat32)
            pszFieldType = "REAL";
        else if (oField.GetWidth() > 0 && oField.GetPrecision() > 0 &&
                 bPreservePrecision)
            pszFieldType = CPLSPrintf("NUMERIC(%d,%d)", oField.GetWidth(),
                                      oField.GetPrecision());
        else
            pszFieldType = "DOUBLE";
    }
    else if (oField.GetType() == OFTString)
    {
        if (oField.GetWidth() > 0 && oField.GetWidth() < 10485760 &&
            bPreservePrecision)
            pszFieldType = CPLSPrintf("VARCHAR(%d)", oField.GetWidth());
        else
            pszFieldType = "varchar";
    }
    else if (oField.GetType() == OFTDate)
    {
        pszFieldType = "date";
    }
    else if (oField.GetType() == OFTTime)
    {
        pszFieldType = "time";
    }
    else if (oField.GetType() == OFTDateTime)
    {
        pszFieldType = "timestamp with time zone";
    }
    else if (oField.GetType() == OFTBinary)
    {
        pszFieldType = "varbinary";
    }
    else if (bApproxOK)
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Can't create field %s with type %s on Dameng layers.  "
                 "Creating as VARCHAR.",
                 oField.GetNameRef(),
                 OGRFieldDefn::GetFieldTypeName(oField.GetType()));
        pszFieldType = "VARCHAR";
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Can't create field %s with type %s on Dameng layers.",
                 oField.GetNameRef(),
                 OGRFieldDefn::GetFieldTypeName(oField.GetType()));
    }

    return pszFieldType;
}

/************************************************************************/
/*                           OGRDMCheckType()                           */
/************************************************************************/

OGRwkbGeometryType OGRDMCheckType(int typid)

{
    switch (typid)
    {
        case NDCT_CLSID_GEO2_ST_GEOMETRY:
            return OGRFromOGCGeomType("GEOMETRY");
        case NDCT_CLSID_GEO2_ST_POINT:
            return OGRFromOGCGeomType("POINT");
        case NDCT_CLSID_GEO2_ST_LINE:
            return OGRFromOGCGeomType("LINESTRING");
        case NDCT_CLSID_GEO2_ST_POLYGON:
            return OGRFromOGCGeomType("POLYGON");
        case NDCT_CLSID_GEO2_ST_MULTIPOINT:
            return OGRFromOGCGeomType("MULTIPOINT");
        case NDCT_CLSID_GEO2_ST_MULTILINE:
            return OGRFromOGCGeomType("MULTILINESTRING");
        case NDCT_CLSID_GEO2_ST_MULTIPOLYGON:
            return OGRFromOGCGeomType("MULTIPOLYGON");
        case NDCT_CLSID_GEO2_ST_COLLECTION:
            return OGRFromOGCGeomType("GEOMETRYCOLLECTION");
        case NDCT_CLSID_GEO2_ST_CIRCSTRING:
            return OGRFromOGCGeomType("CIRCULARSTRING");
        case NDCT_CLSID_GEO2_ST_COMPOUND:
            return OGRFromOGCGeomType("COMPOUNDCURVE");
        case NDCT_CLSID_GEO2_ST_CURVEPOLY:
            return OGRFromOGCGeomType("CURVEPOLYGON");
        case NDCT_CLSID_GEO2_ST_MULTICURVE:
            return OGRFromOGCGeomType("MULTICURVE");
        case NDCT_CLSID_GEO2_ST_MULTISURFACE:
            return OGRFromOGCGeomType("MULTISURFACE");
        case NDCT_CLSID_GEO2_ST_POLYHEDRALSURFACE:
            return OGRFromOGCGeomType("POLYHEDRALSURFACE");
        case NDCT_CLSID_GEO2_ST_TRIANGLE:
            return OGRFromOGCGeomType("TRIANGLE");
        case NDCT_CLSID_GEO2_ST_TIN:
            return OGRFromOGCGeomType("TIN");
        case NDCT_CLSID_GEO2_ST_GEOGRAPHY:
            return OGRFromOGCGeomType("GEOGRAPHY");
        default:
            return OGRFromOGCGeomType("UNKNOWN");
    }
}
