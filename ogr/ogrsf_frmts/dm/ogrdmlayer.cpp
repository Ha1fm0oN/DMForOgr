#include "ogr_dm.h"
#include "cpl_conv.h"
#include "ogr_p.h"

CPL_CVSID("$Id$")

/************************************************************************/
/*                           OGRDMLayer()                               */
/************************************************************************/

OGRDMLayer::OGRDMLayer()

{
    poFeatureDefn = nullptr;
    poDS = nullptr;
    poStatement = nullptr;
    pszQueryStatement = nullptr;
    nResultOffset = 0;
    pszFIDColumn = nullptr;
}

/************************************************************************/
/*                            ~OGRDMLayer()                             */
/************************************************************************/

OGRDMLayer::~OGRDMLayer()

{
    if (m_nFeaturesRead > 0 && poFeatureDefn != nullptr)
    {
        CPLDebug("DM", "%lld features read on layer '%s'.", m_nFeaturesRead,
                 poFeatureDefn->GetName());
    }
    OGRDMLayer::ResetReading();

    CPLFree(pszFIDColumn);
    CPLFree(pszQueryStatement);

    if (poStatement != nullptr)
        delete poStatement;

    if (poFeatureDefn != nullptr)
        poFeatureDefn->Release();

    if (m_panMapFieldNameToIndex != nullptr)
        CPLFree(m_panMapFieldNameToIndex);
    if (m_panMapFieldNameToGeomIndex != nullptr)
        CPLFree(m_panMapFieldNameToGeomIndex);
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRDMLayer::ResetReading()

{
    GetLayerDefn();

    iNextShapeId = 0;
}

/************************************************************************/
/*                          RecordToFeature()                           */
/*                                                                      */
/*      Convert the indicated record of the current result set into     */
/*      a feature.                                                      */
/************************************************************************/

OGRFeature *OGRDMLayer::RecordToFeature(OGRDMStatement *hStmt,
                                        const int *panMapFieldNameToIndex,
                                        const int *panMapFieldNameToGeomIndex,
                                        int iRecord)
{
    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    sdint2 rs_deci = 0;
    ulength rs_size = 0;
    sdint2 rs_null = 0;
    sdint2 rs_type = 0;
    sdint2 rs_name_len = 0;
    sdint2 name_max = 200;
    DPIRETURN rt;

    poFeature->SetFID(iNextShapeId);
    m_nFeaturesRead++;
    /* -------------------------------------------------------------------- */
    /*      Handle FID.                                                     */
    /* -------------------------------------------------------------------- */

    for (int iField = 0; iField < hStmt->GetColCount(); iField++)
    {
        sdbyte pszFieldName[200];
        rt = dpi_desc_column(*hStmt->GetStatement(), (sdint2)iField + 1,
                             pszFieldName, name_max, &rs_name_len, &rs_type,
                             &rs_size, &rs_deci, &rs_null);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error!");
            return NULL;
        }
        char *pabyData = result[iField][iRecord];
        if (pszFIDColumn != nullptr &&
            EQUAL((const char *)pszFieldName, pszFIDColumn))
        {
            if (pabyData)
                poFeature->SetFID(CPLAtoGIntBig(pabyData));
            else
                continue;
        }

        /* -------------------------------------------------------------------- */
        /*      Handle geometry                                         */
        /* -------------------------------------------------------------------- */
        int iOGRGeomField =
            panMapFieldNameToGeomIndex ? panMapFieldNameToGeomIndex[iField] : 0;
        OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;

        if (iOGRGeomField >= 0)
        {
            poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(iOGRGeomField);
        }
        if (poGeomFieldDefn &&
            (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY ||
             poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY))
        {
            if (STARTS_WITH_CI((const char *)pszFieldName,
                               "DMGEO2.ST_AsBinary"))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "We cannot handle binary type!");
                return NULL;
            }
            else if (!poDS->bUseBinaryCursor &&
                     STARTS_WITH_CI((const char *)pszFieldName,
                                    "DMGEO2.ST_AsEWKB"))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "We cannot handle binary type!");
                return NULL;
            }
            else if (STARTS_WITH_CI((const char *)pszFieldName,
                                    "DMGEO2.ST_ASTEXT"))
            {
                /* Handle WKT */
                const char *pszWKT = pabyData;
                const char *pszSRID = pszWKT;
                if (STARTS_WITH_CI(pszSRID, "SRID="))
                {
                    while (*pszSRID != '\0' && *pszSRID != ';')
                        pszSRID++;
                    if (*pszSRID == ';')
                        pszSRID++;
                }

                OGRGeometry *poGeometry = nullptr;
                OGRGeometryFactory::createFromWkt(pszWKT, nullptr, &poGeometry);
                if (poGeometry != nullptr)
                {
                    poGeometry->assignSpatialReference(
                        poGeomFieldDefn->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeometry);
                }
                continue;
            }
            else
            {
                if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY ||
                    poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY)
                {
                    OGRGeometry *poGeometry = nullptr;
                    pabyData = result[iField][iRecord];
                    if (pabyData)
                    {
                        ulint64 length;
                        byte *pabyVal = dm_gser_to_wkb((GSERIALIZED *)pabyData,
                                                       &length, nullptr);
                        OGRGeometryFactory::createFromWkb(pabyVal, nullptr,
                                                          &poGeometry, length,
                                                          wkbVariantOldOgc);
                        CPLFree(pabyVal);
                        if (poGeometry != nullptr)
                        {
                            poGeometry->assignSpatialReference(
                                poGeomFieldDefn->GetSpatialRef());
                            poFeature->SetGeomFieldDirectly(iOGRGeomField,
                                                            poGeometry);
                        }
                        continue;
                    }
                    else
                    {
                        poGeometry = nullptr;
                        continue;
                    }
                }
            }
        }

        /* -------------------------------------------------------------------- */
        /*      Transfer regular data fields.                                   */
        /* -------------------------------------------------------------------- */
        const int iOGRField = panMapFieldNameToIndex[iField];

        if (iOGRField < 0)
            continue;
        pabyData = result[iField][iRecord];
        if (!pabyData)
        {
            poFeature->SetFieldNull(iOGRField);
            continue;
        }
        poFeature->SetField(iOGRField, pabyData);
    }
    return poFeature;
}

/************************************************************************/
/*                    OGRDMIsKnownGeomFuncPrefix()                      */
/************************************************************************/

static const char *const apszKnownGeomFuncPrefixes[] = {
    "DMGEO2.ST_AsBinary", "DMGEO2.ST_AsEWKT", "DMGEO2.ST_AsEWKB",
    "DMGEO2.ST_AsText"};
static int OGRDMIsKnownGeomFuncPrefix(const char *pszFieldName)
{
    for (size_t i = 0; i < sizeof(apszKnownGeomFuncPrefixes) / sizeof(char *);
         i++)
    {
        if (EQUALN(pszFieldName, apszKnownGeomFuncPrefixes[i],
                   static_cast<int>(strlen(apszKnownGeomFuncPrefixes[i]))))
            return static_cast<int>(i);
    }
    return -1;
}

/************************************************************************/
/*                CreateMapFromFieldNameToIndex()                       */
/************************************************************************/

void OGRDMLayer::CreateMapFromFieldNameToIndex(OGRDMStatement *hStmt,
                                               OGRFeatureDefn *poFeatureDefn,
                                               int *&panMapFieldNameToIndex,
                                               int *&panMapFieldNameToGeomIndex)
{
    CPLFree(panMapFieldNameToIndex);
    panMapFieldNameToIndex = nullptr;
    CPLFree(panMapFieldNameToGeomIndex);
    panMapFieldNameToGeomIndex = nullptr;

    sdbyte pszName[200];
    sdint2 rs_deci = 0;
    ulength rs_size = 0;
    sdint2 rs_null = 0;
    sdint2 rs_type = 0;
    sdint2 rs_name_len = 0;
    sdint2 name_max = 200;
    DPIRETURN rt;
    sdint2 nColumns = 0;

    rt = dpi_number_columns(*hStmt->GetStatement(), &nColumns);
    if (DSQL_SUCCEEDED(rt))
    {
        panMapFieldNameToIndex =
            static_cast<int *>(CPLMalloc(sizeof(int) * nColumns));
        panMapFieldNameToGeomIndex =
            static_cast<int *>(CPLMalloc(sizeof(int) * nColumns));
        for (int iField = 0; iField < nColumns; iField++)
        {
            rt = dpi_desc_column(*hStmt->GetStatement(), (sdint2)iField + 1,
                                 pszName, name_max, &rs_name_len, &rs_type,
                                 &rs_size, &rs_deci, &rs_null);
            panMapFieldNameToIndex[iField] =
                poFeatureDefn->GetFieldIndex((const char *)pszName);
            if (panMapFieldNameToIndex[iField] < 0)
            {
                panMapFieldNameToGeomIndex[iField] =
                    poFeatureDefn->GetGeomFieldIndex((const char *)pszName);
                if (panMapFieldNameToGeomIndex[iField] < 0)
                {
                    int iKnownPrefix =
                        OGRDMIsKnownGeomFuncPrefix((const char *)pszName);
                    if (iKnownPrefix >= 0 &&
                        pszName[strlen(
                            apszKnownGeomFuncPrefixes[iKnownPrefix])] ==
                            '_')
                    {
                        panMapFieldNameToGeomIndex[iField] =
                            poFeatureDefn->GetGeomFieldIndex(
                                (const char *)pszName +
                                strlen(
                                    apszKnownGeomFuncPrefixes[iKnownPrefix]) +
                                1);
                    }
                }
            }
            else
                panMapFieldNameToGeomIndex[iField] = -1;
        }
    }
}

/************************************************************************/
/*                     SetInitialQuery()                          */
/************************************************************************/

void OGRDMLayer::SetInitialQuery()
{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    poStatement = new OGRDMStatement(hDMConn);
    CPLString osCommand;

    CPLAssert(pszQueryStatement != nullptr);
    osCommand.Printf("%s", pszQueryStatement);
    CPLErr rt = poStatement->Excute_for_fetchmany(osCommand.c_str());

    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DM:Execute command failure!");
    }

    CreateMapFromFieldNameToIndex(poStatement, poFeatureDefn,
                                  m_panMapFieldNameToIndex,
                                  m_panMapFieldNameToGeomIndex);

    nResultOffset = 0;
}

/************************************************************************/
/*                         GetNextRawFeature()                          */
/************************************************************************/

OGRFeature *OGRDMLayer::GetNextRawFeature()

{
    OGRFeature *poFeature = nullptr;
    if (iNextShapeId == 0)
    {
        SetInitialQuery();
        result = poStatement->Fetchmany(&rows);
        total_rows = rows;
        if (rows < fetchnum)
            isfetchall = 1;
    }
    if (rows > 0)
    {
        poFeature = RecordToFeature(poStatement, m_panMapFieldNameToIndex,
                                    m_panMapFieldNameToGeomIndex,
                                    (int)(total_rows - rows));
        rows--;
    }
    else if (isfetchall == 0)
    {
        result = poStatement->Fetchmany(&rows);
        total_rows = rows;
        if (rows < fetchnum)
            isfetchall = 1;
        if (rows == 0)
            poFeature = nullptr;
        else
        {
            poFeature = RecordToFeature(poStatement, m_panMapFieldNameToIndex,
                                        m_panMapFieldNameToGeomIndex,
                                        (int)(total_rows - rows));
            rows--;
        }
    }
    else
    {
        poFeature = nullptr;
    }
    nResultOffset++;
    iNextShapeId++;
    return poFeature;
}

/************************************************************************/
/*                           SetNextByIndex()                           */
/************************************************************************/

OGRErr OGRDMLayer::SetNextByIndex(GIntBig nIndex)

{
    GetLayerDefn();

    if (!TestCapability(OLCFastSetNextByIndex))
        return OGRLayer::SetNextByIndex(nIndex);

    if (nIndex == iNextShapeId)
    {
        return OGRERR_NONE;
    }

    if (nIndex < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid index");
        return OGRERR_FAILURE;
    }

    if (nIndex == 0)
    {
        ResetReading();
        return OGRERR_NONE;
    }

    sdint2 nParams;
    CPLString osCommand;
    DPIRETURN rt;
    rt = dpi_number_params(poStatement->GetStatement(), &nParams);
    SetInitialQuery();

    poStatement->SimpleFetchRow();
    if (!DSQL_SUCCEEDED(rt) || nParams != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to read feature at invalid index (" CPL_FRMT_GIB ").",
                 nIndex);
        iNextShapeId = 0;

        return OGRERR_FAILURE;
    }

    nResultOffset = 0;
    iNextShapeId = (int)nIndex;

    return OGRERR_NONE;
}

/************************************************************************/
/*                        BlobToGByteArray()                           */
/************************************************************************/

GByte *OGRDMLayer::BlobToGByteArray(const char *pszBlob, int *pnLength)
{
    if (pszBlob == nullptr)
    {
        if (pnLength)
            *pnLength = 0;
        return nullptr;
    }

    return CPLHexToBinary(pszBlob + 2, pnLength);
}

/************************************************************************/
/*                          BlobToGeometry()                           */
/************************************************************************/

OGRGeometry *OGRDMLayer::BlobToGeometry(const char *pszBlob)

{
    if (pszBlob == nullptr)
        return nullptr;

    int nLen = 0;
    GByte *pabyWKB = BlobToGByteArray(pszBlob, &nLen);

    OGRGeometry *poGeometry = nullptr;
    OGRGeometryFactory::createFromWkb(pabyWKB, nullptr, &poGeometry, nLen,
                                      wkbVariantPostGIS1);

    CPLFree(pabyWKB);
    return poGeometry;
}

/************************************************************************/
/*                        GByteArrayToBlob()                           */
/************************************************************************/

char *OGRDMLayer::GByteArrayToBlob(const GByte *pabyData, size_t nLen)
{
    if (nLen > (std::numeric_limits<size_t>::max() - 1) / 5)
        return CPLStrdup("");
    const size_t nTextBufLen = nLen * 5 + 1;
    char *pszTextBuf = static_cast<char *>(VSI_MALLOC_VERBOSE(nTextBufLen));
    if (pszTextBuf == nullptr)
        return CPLStrdup("");

    size_t iDst = 0;

    for (size_t iSrc = 0; iSrc < nLen; iSrc++)
    {
        if (pabyData[iSrc] < 40 || pabyData[iSrc] > 126 ||
            pabyData[iSrc] == '\\')
        {
            snprintf(pszTextBuf + iDst, nTextBufLen - iDst, "\\\\%03o",
                     pabyData[iSrc]);
            iDst += 5;
        }
        else
            pszTextBuf[iDst++] = pabyData[iSrc];
    }
    pszTextBuf[iDst] = '\0';

    return pszTextBuf;
}

/************************************************************************/
/*                          GeometryToBlob()                           */
/************************************************************************/

char *OGRDMLayer::GeometryToBlob(const OGRGeometry *poGeometry)

{
    const size_t nWkbSize = poGeometry->WkbSize();

    GByte *pabyWKB = static_cast<GByte *>(VSI_MALLOC_VERBOSE(nWkbSize));
    if (pabyWKB == nullptr)
        return CPLStrdup("");

    if (wkbFlatten(poGeometry->getGeometryType()) == wkbPoint &&
        poGeometry->IsEmpty())
    {
        if (poGeometry->exportToWkb(wkbNDR, pabyWKB, wkbVariantIso) !=
            OGRERR_NONE)
        {
            CPLFree(pabyWKB);
            return CPLStrdup("");
        }
    }
    else if (poGeometry->exportToWkb(wkbNDR, pabyWKB, wkbVariantOldOgc) !=
             OGRERR_NONE)
    {
        CPLFree(pabyWKB);
        return CPLStrdup("");
    }

    char *pszTextBuf = GByteArrayToBlob(pabyWKB, nWkbSize);
    CPLFree(pabyWKB);

    return pszTextBuf;
}
/************************************************************************/
/*                            GetFIDColumn()                            */
/************************************************************************/

const char *OGRDMLayer::GetFIDColumn()

{
    if (pszFIDColumn != nullptr)
        return pszFIDColumn;
    else
        return "";
}

/************************************************************************/
/*                             GetExtent()                              */
/*                                                                      */
/*      For DMGEO2 use internal Extend(geometry) function               */
/*      in other cases we use standard OGRLayer::GetExtent()            */
/************************************************************************/

OGRErr OGRDMLayer::GetExtent(int iGeomField, OGREnvelope *psExtent,
                             int bForce)
{

    if (iGeomField == 0)
        return OGRLayer::GetExtent(psExtent, bForce);
    else
        return OGRLayer::GetExtent(iGeomField, psExtent, bForce);
}

/************************************************************************/
/*                        RunGetExtentRequest()                         */
/************************************************************************/

OGRErr OGRDMLayer::RunGetExtentRequest(OGREnvelope *psExtent,
                                       CPL_UNUSED int bForce,
                                       CPLString osCommand,
                                       int bErrorAsDebug)
{
    if (psExtent == nullptr)
        return OGRERR_FAILURE;

    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement *hStmt = new OGRDMStatement(hDMConn);
    CPLErr eErr = hStmt->Execute(osCommand);
    if (!hStmt || eErr)
    {
        CPLDebug("DM", "Unable to get extent by DMGEO2");
        return OGRERR_FAILURE;
    }

    char *pszBox = hStmt->SimpleFetchRow()[0];
    char *ptr, *ptrEndParenthesis;
    char szVals[64 * 6 + 6];

    ptr = strchr(pszBox, '(');
    if (ptr)
        ptr++;
    if (ptr == nullptr || (ptrEndParenthesis = strchr(ptr, ')')) == nullptr ||
        ptrEndParenthesis - ptr > static_cast<int>(sizeof(szVals) - 1))
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Bad extent representation: '%s'",
                 pszBox);
        return OGRERR_FAILURE;
    }

    strncpy(szVals, ptr, ptrEndParenthesis - ptr);
    szVals[ptrEndParenthesis - ptr] = '\0';

    char **papszTokens = CSLTokenizeString2(szVals, " ,", CSLT_HONOURSTRINGS);

    psExtent->MinX = CPLAtof(papszTokens[0]);
    psExtent->MinY = CPLAtof(papszTokens[1]);
    psExtent->MaxX = CPLAtof(papszTokens[2]);
    psExtent->MaxY = CPLAtof(papszTokens[3]);

    CSLDestroy(papszTokens);
    hStmt->Clean();

    return OGRERR_NONE;
}

/************************************************************************/
/*                        ReadResultDefinition()                        */
/*                                                                      */
/*      Build a schema from the current resultset.                      */
/************************************************************************/

int OGRDMLayer::ReadResultDefinition(OGRDMStatement *hInitialResultIn)

{
    OGRDMStatement *hStmt = hInitialResultIn;
    sdbyte columnName[200];
    sdint2 rs_deci = 0;
    ulength rs_size = 0;
    sdint2 rs_null = 0;
    sdint2 rs_type = 0;
    sdint2 rs_name_len = 0;
    sdint2 name_max = 199;
    DPIRETURN rt;
    dhdesc desc;

    poFeatureDefn = new OGRDMFeatureDefn("sql_statement");
    SetDescription(poFeatureDefn->GetName());

    poFeatureDefn->Reference();

    sdint2 nColumns = 0;

    rt = dpi_number_columns(*hStmt->GetStatement(), &nColumns);
    if (rt)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get col numbers");
        return FALSE;
    }
    rt = dpi_get_stmt_attr(*hStmt->GetStatement(), DSQL_ATTR_IMP_ROW_DESC,
                           &desc, sizeof(desc), NULL);
    for (int iRawField = 0; iRawField < nColumns; iRawField++)
    {
        sdint2 type = 0;
        udint4 obj_classid = 0;
        slength display_size = 0;

        rt = dpi_desc_column(*hStmt->GetStatement(), (sdint2)iRawField + 1,
                             columnName, name_max, &rs_name_len, &rs_type,
                             &rs_size, &rs_deci, &rs_null);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get col_desc");
            return FALSE;
        }

        rt = dpi_get_desc_field(desc, (sdint2)iRawField + 1,
                                DSQL_DESC_CONCISE_TYPE, &type, sizeof(sdint2),
                                NULL);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get type");
            return FALSE;
        }

        rt = dpi_get_desc_field(desc, (sdint2)iRawField + 1,
                                DSQL_DESC_DISPLAY_SIZE, &display_size,
                                sizeof(slength), NULL);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get display_size");
            return FALSE;
        }

        if (type == DSQL_CLASS)
        {
            rt = dpi_get_desc_field(desc, (sdint2)iRawField + 1,
                                    DSQL_DESC_OBJ_CLASSID, &obj_classid,
                                    sizeof(udint4), NULL);
        }
        OGRFieldDefn oField((const char *)columnName, OFTString);
        oField.SetNullable(rs_null);

        int iGeomFuncPrefix = 0;
        if (EQUAL(oField.GetNameRef(), "ogc_fid"))
        {
            if (pszFIDColumn)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "More than one ogc_fid column was found in the result "
                         "of the SQL request. Only last one will be used");
            }
            CPLFree(pszFIDColumn);
            pszFIDColumn = CPLStrdup(oField.GetNameRef());
            continue;
        }
        else if ((iGeomFuncPrefix =
                      OGRDMIsKnownGeomFuncPrefix(oField.GetNameRef())) >= 0 ||
                 (obj_classid >= NDCT_CLSID_GEO2_ST_GEOMETRY &&
                  obj_classid <= NDCT_CLSID_GEO2_ST_GEOGRAPHY))
        {
            auto poGeomFieldDefn =
                std::make_unique<OGRDMGeomFieldDefn>(this, oField.GetNameRef());
            if (iGeomFuncPrefix >= 0 &&
                oField.GetNameRef()[strlen(
                    apszKnownGeomFuncPrefixes[iGeomFuncPrefix])] == '_')
            {
                poGeomFieldDefn->SetName(
                    oField.GetNameRef() +
                    strlen(apszKnownGeomFuncPrefixes[iGeomColumn]) + 1);
            }

            if (obj_classid == NDCT_CLSID_GEO2_ST_GEOGRAPHY)
            {
                poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOGRAPHY;
            }
            else
                poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOMETRY;

            poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
            continue;
        }
        else if (EQUAL(oField.GetNameRef(), "WKB_GEOMETRY"))
        {
            auto poGeomFieldDefn = std::make_unique<OGRDMGeomFieldDefn>(
                this, oField.GetNameRef());
            poGeomFieldDefn->eDMGeoType = GEOM_TYPE_WKB;
            poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
            continue;
        }

        if (type == DSQL_CLASS)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unsupported type to export. ");
            return FALSE;
        }

        if (rs_type == DSQL_BLOB || rs_type == DSQL_CLOB ||
            rs_type == DSQL_BINARY || rs_type == DSQL_VARBINARY)
        {
            oField.SetType(OFTBinary);
        }
        else if (rs_type == DSQL_CHAR || rs_type == DSQL_CLOB ||
                 rs_type == DSQL_VARCHAR)
        {
            oField.SetType(OFTString);
            oField.SetWidth((int)display_size);
        }
        else if (rs_type == DSQL_BIT)
        {
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTBoolean);
            oField.SetWidth(1);
        }
        else if (rs_type == DSQL_SMALLINT)
        {
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTInt16);
            oField.SetWidth(5);
        }
        else if (rs_type == DSQL_INT)
        {
            oField.SetType(OFTInteger);
        }
        else if (rs_type == DSQL_BIGINT)
        {
            oField.SetType(OFTInteger64);
        }
        else if (rs_type == DSQL_FLOAT)
        {
            oField.SetType(OFTReal);
            oField.SetSubType(OFSTFloat32);
        }
        else if (rs_type == DSQL_DOUBLE)
        {
            oField.SetType(OFTReal);
        }
        else if (rs_type == DSQL_DEC)
        {
            if (rs_deci == 0)
            {
                oField.SetType((rs_size < 10) ? OFTInteger : OFTInteger64);
                if (rs_size < 38)
                    oField.SetWidth((int)rs_size);
            }
            else
            {
                oField.SetType(OFTReal);
                oField.SetWidth((int)rs_size);
                oField.SetPrecision(rs_deci);
            }
        }
        else if (rs_type == DSQL_DATE)
        {
            oField.SetType(OFTDate);
        }
        else if (rs_type == DSQL_TIME)
        {
            oField.SetType(OFTTime);
        }
        else if (rs_type == DSQL_TIMESTAMP || rs_type == DSQL_TIMESTAMP_TZ ||
                 rs_type == DSQL_TIME_TZ)
        {
            oField.SetType(OFTDateTime);
        }
        else /* unknown type */
        {
            CPLDebug("DM",
                     "Unhandled OID (%d) for column %s. Defaulting to String.",
                     rs_type, oField.GetNameRef());
            oField.SetType(OFTString);
        }

        poFeatureDefn->AddFieldDefn(&oField);
    }
    return TRUE;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/
const OGRSpatialReference *OGRDMGeomFieldDefn::GetSpatialRef() const
{
    if (poLayer == nullptr)
        return nullptr;
    if (nSRSId == UNDETERMINED_SRID)
        poLayer->ResolveSRID(this);
    if (poSRS == nullptr && nSRSId > 0)
    {
        poSRS = poLayer->GetDS()->FetchSRS(nSRSId);
        if (poSRS != nullptr)
            const_cast<OGRSpatialReference *>(poSRS)->Reference();
    }
    return poSRS;
}

/************************************************************************/
/*                          AppendFieldValue()                          */
/*                                                                      */
/* Used by CreateFeatureViaInsert() and SetFeature() to format a        */
/* non-empty field value                                                */
/************************************************************************/

void OGRDMCommonAppendFieldValue(CPLString &osCommand,
                                 OGRFeature *poFeature,
                                 int i)
{
    if (poFeature->IsFieldNull(i))
    {
        osCommand += "NULL";
        return;
    }

    OGRFeatureDefn *poFeatureDefn = poFeature->GetDefnRef();
    OGRFieldType nOGRFieldType = poFeatureDefn->GetFieldDefn(i)->GetType();
    OGRFieldSubType eSubType = poFeatureDefn->GetFieldDefn(i)->GetSubType();

    // Flag indicating NULL or not-a-date date value
    // e.g. 0000-00-00 - there is no year 0
    bool bIsDateNull = false;

    const char *pszStrValue = poFeature->GetFieldAsString(i);

    // Check if date is NULL: 0000-00-00
    if (nOGRFieldType == OFTDate)
    {
        if (STARTS_WITH_CI(pszStrValue, "0000"))
        {
            pszStrValue = "NULL";
            bIsDateNull = true;
        }
    }
    else if (nOGRFieldType == OFTReal)
    {
        //Check for special values. They need to be quoted.
        double dfVal = poFeature->GetFieldAsDouble(i);
        if (CPLIsNan(dfVal))
            pszStrValue = "'NaN'";
        else if (CPLIsInf(dfVal))
            pszStrValue = (dfVal > 0) ? "'Infinity'" : "'-Infinity'";
    }
    else if ((nOGRFieldType == OFTInteger || nOGRFieldType == OFTInteger64) &&
             eSubType == OFSTBoolean)
        pszStrValue = poFeature->GetFieldAsInteger(i) ? "1" : "0";

    osCommand += "'";
    osCommand += pszStrValue;
    osCommand += "'";
}
