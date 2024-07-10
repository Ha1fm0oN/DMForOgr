#include <string.h>
#include "ogr_dm.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_hash_set.h"
#include <cctype>
#include <set>

CPL_CVSID("$Id$")


int ogr_DM_insertnum = FORCED_INSERT_NUM;
/*
OGRDMDataSource()
*/
OGRDMDataSource::OGRDMDataSource() = default;

//~OGRDMDataSource
OGRDMDataSource::~OGRDMDataSource()

{
    OGRDMDataSource::FlushCache(true);

    CPLFree(pszName);
    CPLFree(pszForcedTables);
    CSLDestroy(papszSchemaList);

    for (int i = 0; i < nLayers; i++)
        delete papoLayers[i];
    
    CPLFree(papoLayers);

    for (int i = 0; i < nKnownSRID; i++)
    {
        if (papoSRS[i] != nullptr)//
            papoSRS[i]->Release();//
    }
    CPLFree(panSRID);
    CPLFree(papoSRS);
    delete poSession;
}

typedef struct
{
    char *pszTableName;
    char *pszSchemaName;
    char *pszDescription;
    int nGeomColumnCount;
    DMGeomColumnDesc *pasGeomColumns; /* list of geometry columns */
    int bDerivedInfoAdded; /* set to TRUE if it derives from another table */
} DMTableEntry;

static unsigned long OGRDMHashTableEntry(const void *_psTableEntry)
{
    const DMTableEntry *psTableEntry =
        static_cast<const DMTableEntry *>(_psTableEntry);
    return CPLHashSetHashStr(CPLString().Printf(
        "%s.%s", psTableEntry->pszSchemaName, psTableEntry->pszTableName));
}

static int OGRDMEqualTableEntry(const void *_psTableEntry1,
                                const void *_psTableEntry2)
{
    const DMTableEntry *psTableEntry1 =
        static_cast<const DMTableEntry *>(_psTableEntry1);
    const DMTableEntry *psTableEntry2 =
        static_cast<const DMTableEntry *>(_psTableEntry2);
    return strcmp(psTableEntry1->pszTableName, psTableEntry2->pszTableName) ==
               0 &&
           strcmp(psTableEntry1->pszSchemaName, psTableEntry2->pszSchemaName) ==
               0;
}

static void OGRDMFreeTableEntry(void *_psTableEntry)
{
    DMTableEntry *psTableEntry = static_cast<DMTableEntry *>(_psTableEntry);
    CPLFree(psTableEntry->pszTableName);
    CPLFree(psTableEntry->pszSchemaName);
    CPLFree(psTableEntry->pszDescription);
    for (int i = 0; i < psTableEntry->nGeomColumnCount; i++)
    {
        CPLFree(psTableEntry->pasGeomColumns[i].pszName);
        CPLFree(psTableEntry->pasGeomColumns[i].pszGeomType);
    }
    CPLFree(psTableEntry->pasGeomColumns);
    CPLFree(psTableEntry);
}

static void OGRDMTableEntryAddGeomColumn(DMTableEntry *psTableEntry,
                                         const char *pszName,
                                         const char *pszGeomType = nullptr,
                                         int GeometryTypeFlags = 0,
                                         int nSRID = UNDETERMINED_SRID,
                                         DMGeoType eDMType = GEOM_TYPE_UNKNOWN,
                                         int bNullable = TRUE)
{
    psTableEntry->pasGeomColumns = static_cast<DMGeomColumnDesc *>(CPLRealloc(
        psTableEntry->pasGeomColumns,
        sizeof(DMGeomColumnDesc) * (psTableEntry->nGeomColumnCount + 1)));
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount].pszName =
        CPLStrdup(pszName);
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount].pszGeomType =
        (pszGeomType) ? CPLStrdup(pszGeomType) : nullptr;
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount]
        .GeometryTypeFlags = GeometryTypeFlags;
    /* With dmgeo2, querying geometry_columns can return 0, not only when */
    /* the SRID is truly set to 0, but also when there's no constraint */
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount].nSRID =
        nSRID > 0 ? nSRID : UNDETERMINED_SRID;
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount].eDMGeoType =
        eDMType;
    psTableEntry->pasGeomColumns[psTableEntry->nGeomColumnCount].bNullable =
        bNullable;
    psTableEntry->nGeomColumnCount++;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRDMDataSource::Open(const char *pszNewName,
                          int bUpdate,
                          int bTestOpen,
                          char **papszOpenOptionsIn)

{
    CPLAssert(nLayers == 0);
    papszOpenOptions = CSLDuplicate(papszOpenOptionsIn);

    if (!STARTS_WITH_CI(pszNewName, "DM:"))
    {
        if (!bTestOpen)
            CPLError(CE_Failure, CPLE_AppDefined,
                     "%s does not conform to DM naming convention,"
                     " DM:*\n",
                     pszNewName);
        return FALSE;
    }

    char *pszUserid;
    const char *pszPassword = "";
    const char *pszDatabase = "";
    char **papszTableList = nullptr;
    int i;

    if (pszNewName[3] == '\0')
    {
        pszUserid =
            CPLStrdup(CSLFetchNameValueDef(papszOpenOptionsIn, "USER", ""));
        pszPassword = CSLFetchNameValueDef(papszOpenOptionsIn, "PASSWORD", "");
        pszDatabase = CSLFetchNameValueDef(papszOpenOptionsIn, "DBNAME", "");
        const char *pszTables = CSLFetchNameValue(papszOpenOptionsIn, "TABLES");
        if (pszTables)
            papszTableList =
                CSLTokenizeStringComplex(pszTables, ",", TRUE, FALSE);
    }
    else
    {
        pszUserid = CPLStrdup(pszNewName + 3);

        // Is there a table list?
        for (i = static_cast<int>(strlen(pszUserid)) - 1; i > 1; i--)
        {
            if (pszUserid[i] == ';')
            {
                ogr_DM_insertnum = atoi(pszUserid + i + 1);
                pszUserid[i] = '\0';
                break;
            }
        }

        for (i = 0;
             pszUserid[i] != '\0' && pszUserid[i] != '/' && pszUserid[i] != '@';
             i++)
        {
        }

        if (pszUserid[i] == '/')
        {
            pszUserid[i++] = '\0';
            pszPassword = pszUserid + i;
            for (; pszUserid[i] != '\0' && pszUserid[i] != '@'; i++)
            {
            }
        }

        if (pszUserid[i] == '@')
        {
            pszUserid[i++] = '\0';
            pszDatabase = pszUserid + i;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Try to establish connection.                                    */
    /* -------------------------------------------------------------------- */

    poSession = OGRGetDMConnection(pszUserid, pszPassword, pszDatabase);

    osCurrentSchema = "SYSDBA";
    if (poSession == nullptr)
    {
        CPLFree(pszUserid);
        CSLDestroy(papszTableList);
        return FALSE;
    }

    bDSUpdate = bUpdate;

    DMTableEntry **papsTables = nullptr;
    int nTableCount = 0;
    CPLHashSet *hSetTables = nullptr;
    std::set<CPLString> osRegisteredLayers;

    for (i = 0; i < nLayers; i++)
    {
        osRegisteredLayers.insert(papoLayers[i]->GetName());
    }
    if (papszTableList == nullptr)
    {
        OGRDMStatement oGetTbales(poSession);
        if (oGetTbales.Execute("SELECT DISTINCT F_TABLE_NAME,F_TABLE_SCHEMA "
                               "FROM SYSGEO2.GEOMETRY_COLUMNS ") == CE_None)
        {
            char **row = nullptr;
            while ((row = oGetTbales.SimpleFetchRow()) != nullptr)
            {
                char tablename[100];
                snprintf(tablename, sizeof(tablename), "%s.%s", row[1], row[0]);
                if (CSLFindString(papszTableList, tablename) == -1)
                {
                    papszTableList = CSLAddString(papszTableList, tablename);
                }
            }
        }
        if (oGetTbales.Execute("SELECT DISTINCT F_TABLE_NAME,F_TABLE_SCHEMA "
                               "FROM SYSGEO2.GEOGRAPHY_COLUMNS ") == CE_None)
        {
            char **row = nullptr;
            while ((row = oGetTbales.SimpleFetchRow()) != nullptr)
            {
                char tablename[100];
                snprintf(tablename, sizeof(tablename), "%s.%s", row[1], row[0]);
                if (CSLFindString(papszTableList, tablename) == -1)
                {
                    papszTableList = CSLAddString(papszTableList, tablename);
                }
            }
        }
    }

    if (papszTableList)
    {
        for (i = 0; i < CSLCount(papszTableList); i++)
        {
            // Get schema and table name
            char **papszQualifiedParts =
                CSLTokenizeString2(papszTableList[i], ".", 0);
            int nParts = CSLCount(papszQualifiedParts);

            if (nParts == 1 || nParts == 2)
            {
                /* Find the geometry column name if specified */
                char *pszGeomColumnName = nullptr;
                char *pos = strchr(
                    papszQualifiedParts[CSLCount(papszQualifiedParts) - 1],
                    '(');
                if (pos != nullptr)
                {
                    *pos = '\0';
                    pszGeomColumnName = pos + 1;
                    int len = static_cast<int>(strlen(pszGeomColumnName));
                    if (len > 0)
                        pszGeomColumnName[len - 1] = '\0';
                }

                papsTables = static_cast<DMTableEntry **>(CPLRealloc(
                    papsTables, sizeof(DMTableEntry *) * (nTableCount + 1)));
                papsTables[nTableCount] = static_cast<DMTableEntry *>(
                    CPLCalloc(1, sizeof(DMTableEntry)));
                if (pszGeomColumnName)
                    OGRDMTableEntryAddGeomColumn(papsTables[nTableCount],
                                                 pszGeomColumnName);

                if (nParts == 2)
                {
                    papsTables[nTableCount]->pszSchemaName =
                        CPLStrdup(papszQualifiedParts[0]);
                    papsTables[nTableCount]->pszTableName =
                        CPLStrdup(papszQualifiedParts[1]);
                }
                else
                {
                    papsTables[nTableCount]->pszSchemaName =
                        CPLStrdup(osActiveSchema.c_str());
                    papsTables[nTableCount]->pszTableName =
                        CPLStrdup(papszQualifiedParts[0]);
                }
                nTableCount++;
            }

            CSLDestroy(papszQualifiedParts);
        }
        CSLDestroy(papszTableList);
    }

    hSetTables = CPLHashSetNew(OGRDMHashTableEntry, OGRDMEqualTableEntry,
                               OGRDMFreeTableEntry);
    for (int iRecord = 0; iRecord < nTableCount; iRecord++)
    {
        const DMTableEntry *psEntry = static_cast<DMTableEntry *>(
            CPLHashSetLookup(hSetTables, papsTables[iRecord]));

        /* If SCHEMAS= is specified, only take into account tables inside */
        /* one of the specified schemas */
        if (papszSchemaList != nullptr &&
            CSLFindString(papszSchemaList,
                          papsTables[iRecord]->pszSchemaName) == -1)
        {
            continue;
        }

        CPLString osDefnName;

        if (papsTables[iRecord]->pszSchemaName &&
            osCurrentSchema != papsTables[iRecord]->pszSchemaName)
        {
            osDefnName.Printf("%s.%s", papsTables[iRecord]->pszSchemaName,
                              papsTables[iRecord]->pszTableName);
        }
        else
        {
            //no prefix for current_schema in layer name, for backwards compatibility
            osDefnName = papsTables[iRecord]->pszTableName;
        }
        if (osRegisteredLayers.find(osDefnName) != osRegisteredLayers.end())
            continue;
        osRegisteredLayers.insert(osDefnName);

        OGRDMTableLayer *poLayer = OpenTable(
            osCurrentSchema, papsTables[iRecord]->pszTableName,
            papsTables[iRecord]->pszSchemaName,
            papsTables[iRecord]->pszDescription, nullptr, bDSUpdate, FALSE);

        if (psEntry != nullptr)
        {
            if (psEntry->nGeomColumnCount > 0)
            {
                poLayer->SetGeometryInformation(psEntry->pasGeomColumns,
                                                psEntry->nGeomColumnCount);
            }
        }
        else
        {
            if (papsTables[iRecord]->nGeomColumnCount > 0)
            {
                poLayer->SetGeometryInformation(
                    papsTables[iRecord]->pasGeomColumns,
                    papsTables[iRecord]->nGeomColumnCount);
            }
        }
    }

    if (hSetTables)
        CPLHashSetDestroy(hSetTables);

    for (i = 0; i < nTableCount; i++)
        OGRDMFreeTableEntry(papsTables[i]);
    CPLFree(papsTables);

    return TRUE;
}

/************************************************************************/
/*                             OpenTable()                              */
/************************************************************************/

OGRDMTableLayer *OGRDMDataSource::OpenTable(CPLString &osCurrentSchemaIn,
                                            const char *pszNewName,
                                            const char *pszSchemaName,
                                            const char *pszDescription,
                                            const char *pszGeomColumnForced,
                                            int bUpdate,
                                            int bTestOpen)

{
    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    OGRDMTableLayer *poLayer =
        new OGRDMTableLayer(this, osCurrentSchemaIn, pszNewName, pszSchemaName,
                            pszDescription, pszGeomColumnForced, bUpdate);
    if (bTestOpen && !(poLayer->ReadTableDefinition()))
    {
        delete poLayer;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Add layer to data source layer list.                            */
    /* -------------------------------------------------------------------- */
    papoLayers = static_cast<OGRDMTableLayer **>(
        CPLRealloc(papoLayers, sizeof(OGRDMTableLayer *) * (nLayers + 1)));
    papoLayers[nLayers++] = poLayer;

    return poLayer;
}

/************************************************************************/
/*                           GetLayerCount()                            */
/************************************************************************/

int OGRDMDataSource::GetLayerCount()
{
    return nLayers;
}

/************************************************************************/
/*                           DeleteLayer()                            */
/************************************************************************/
OGRErr OGRDMDataSource::DeleteLayer(int iLayer)

{
    /* Force loading of all registered tables */
    GetLayerCount();
    if (iLayer < 0 || iLayer >= nLayers)
        return OGRERR_FAILURE;
    /* -------------------------------------------------------------------- */
    /*      Blow away our OGR structures related to the layer.  This is     */
    /*      pretty dangerous if anything has a reference to this layer!     */
    /* -------------------------------------------------------------------- */
    CPLString osLayerName = papoLayers[iLayer]->GetLayerDefn()->GetName();
    CPLString osTableName = papoLayers[iLayer]->GetTableName();
    CPLString osSchemaName = papoLayers[iLayer]->GetSchemaName();

    CPLDebug("DM", "DeleteLayer(%s)", osLayerName.c_str());

    delete papoLayers[iLayer];
    memmove(papoLayers + iLayer, papoLayers + iLayer + 1,
            sizeof(void *) * (nLayers - iLayer - 1));
    nLayers--;

    if (osLayerName.empty())
        return OGRERR_NONE;

    /* -------------------------------------------------------------------- */
    /*      Remove from the database.                                       */
    /* -------------------------------------------------------------------- */
    CPLString osCommand;
    OGRDMStatement oCommand(poSession);

    osCommand.Printf("DROP TABLE \"%s\".\"%s\"", (osSchemaName).c_str(),
                     (osTableName).c_str());
    CPLErr rt = oCommand.Execute(osCommand.c_str());
    if (rt != CE_None)
        return OGRERR_FAILURE;

    return OGRERR_NONE;
}

/************************************************************************/
/*                         OGRDMCommonLaunderName()                     */
/************************************************************************/

char *OGRDMCommonLaunderName(const char *pszSrcName,
                             const char *pszDebugPrefix)

{
    char *pszSafeName = CPLStrdup(pszSrcName);

    for (int i = 0; pszSafeName[i] != '\0'; i++)
    {
        pszSafeName[i] = (char)tolower(pszSafeName[i]);
        if (pszSafeName[i] == '\'' || pszSafeName[i] == '-' ||
            pszSafeName[i] == '#')
        {
            pszSafeName[i] = '_';
        }
    }

    if (strcmp(pszSrcName, pszSafeName) != 0)
        CPLDebug(pszDebugPrefix, "LaunderName('%s') -> '%s'", pszSrcName,
                 pszSafeName);

    return pszSafeName;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *OGRDMDataSource::ICreateLayer(const char *pszLayerName,
                                        const OGRGeomFieldDefn *poGeomFieldDefn,
                                        CSLConstList papszOptions)

{
    CPLString osCommand;
    const char *pszGeomType = nullptr;
    char *pszTableName = nullptr;
    char *pszSchemaName = nullptr;
    int GeometryTypeFlags = 0;

    auto eType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;
    const auto poSRS =
        poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;

    if (pszLayerName == nullptr)
        return nullptr;

    const char *pszFIDColumnNameIn = CSLFetchNameValue(papszOptions, "FID");
    CPLString osFIDColumnName;
    if (pszFIDColumnNameIn == nullptr)
        osFIDColumnName = "ogc_fid";
    else
    {
        if (CPLFetchBool(papszOptions, "LAUNDER", true))
        {
            char *pszLaunderedFid =
                OGRDMCommonLaunderName(pszFIDColumnNameIn, "DM");
            osFIDColumnName += pszLaunderedFid;
            CPLFree(pszLaunderedFid);
        }
        else
            osFIDColumnName += pszFIDColumnNameIn;
    }

    if (STARTS_WITH(pszLayerName, "dm"))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "The layer name should not begin by 'dm' as it is a reserved "
                 "prefix");
    }

    if (OGR_GT_HasZ(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_3D;
    if (OGR_GT_HasM(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_MEASURED;

    int ForcedGeometryTypeFlags = -1;
    const char *pszDim = CSLFetchNameValue(papszOptions, "DIM");
    if (pszDim != nullptr)
    {
        if (EQUAL(pszDim, "XY") || EQUAL(pszDim, "2"))
        {
            GeometryTypeFlags = 0;
            ForcedGeometryTypeFlags = GeometryTypeFlags;
        }
        else if (EQUAL(pszDim, "XYZ") || EQUAL(pszDim, "3"))
        {
            GeometryTypeFlags = OGRGeometry::OGR_G_3D;
            ForcedGeometryTypeFlags = GeometryTypeFlags;
        }
        else if (EQUAL(pszDim, "XYM"))
        {
            GeometryTypeFlags = OGRGeometry::OGR_G_MEASURED;
            ForcedGeometryTypeFlags = GeometryTypeFlags;
        }
        else if (EQUAL(pszDim, "XYZM") || EQUAL(pszDim, "4"))
        {
            GeometryTypeFlags =
                OGRGeometry::OGR_G_3D | OGRGeometry::OGR_G_MEASURED;
            ForcedGeometryTypeFlags = GeometryTypeFlags;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid value for DIM");
        }
    }

    /* Should we turn layers with None geometry type as Unknown/GEOMETRY */
    /* so they are still recorded in geometry_columns table ? (#4012) */
    int bNoneAsUnknown = CPLTestBool(
        CSLFetchNameValueDef(papszOptions, "NONE_AS_UNKNOWN", "NO"));
    if (bNoneAsUnknown && eType == wkbNone)
        eType = wkbUnknown;

    int bExtractSchemaFromLayerName = CPLTestBool(CSLFetchNameValueDef(
        papszOptions, "EXTRACT_SCHEMA_FROM_LAYER_NAME", "YES"));

    const char *pszDotPos = strstr(pszLayerName, ".");
    if (pszDotPos != nullptr && bExtractSchemaFromLayerName)
    {
        int length = static_cast<int>(pszDotPos - pszLayerName);
        pszSchemaName = static_cast<char *>(CPLMalloc(length + 1));
        CPLStrlcpy(pszSchemaName, pszLayerName, length);
        pszSchemaName[length] = '\0';

        if (CPLFetchBool(papszOptions, "LAUNDER", true))
            pszTableName =
                OGRDMCommonLaunderName(pszDotPos + 1, "DM");  //skip "."
        else
            pszTableName = CPLStrdup(pszDotPos + 1);  //skip "."
    }
    else
    {
        pszSchemaName = nullptr;
        if (CPLFetchBool(papszOptions, "LAUNDER", true))
            pszTableName =
                OGRDMCommonLaunderName(pszLayerName, "DM");  //skip "."
        else
            pszTableName = CPLStrdup(pszLayerName);  //skip "."
    }

    /* -------------------------------------------------------------------- */
    /*      Set the default schema for the layers.                          */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszOptions, "SCHEMA") != nullptr)
    {
        CPLFree(pszSchemaName);
        pszSchemaName = CPLStrdup(CSLFetchNameValue(papszOptions, "SCHEMA"));
    }

    if (pszSchemaName == nullptr)
    {
        pszSchemaName = CPLStrdup(osCurrentSchema);
    }

    /* -------------------------------------------------------------------- */
    /*      Do we already have this layer?  If so, should we blow it        */
    /*      away?                                                           */
    /* -------------------------------------------------------------------- */
    CPLString osSQLLayerName;
    if (pszSchemaName == nullptr ||
        (!osCurrentSchema.empty() &&
         EQUAL(pszSchemaName, osCurrentSchema.c_str())))
        osSQLLayerName = pszTableName;
    else
    {
        osSQLLayerName = pszSchemaName;
        osSQLLayerName += ".";
        osSQLLayerName += pszTableName;
    }

    CPLPushErrorHandler(CPLQuietErrorHandler);
    GetLayerByName(osSQLLayerName);
    CPLPopErrorHandler();
    CPLErrorReset();

    /* Force loading of all registered tables */
    GetLayerCount();

    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        if (EQUAL(osSQLLayerName.c_str(), papoLayers[iLayer]->GetName()))
        {
            if (CSLFetchNameValue(papszOptions, "OVERWRITE") != nullptr &&
                !EQUAL(CSLFetchNameValue(papszOptions, "OVERWRITE"), "NO"))
            {
                DeleteLayer(iLayer);
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Layer %s already exists, CreateLayer failed.\n"
                         "Use the layer creation option OVERWRITE=YES to "
                         "replace it.",
                         osSQLLayerName.c_str());
                CPLFree(pszTableName);
                CPLFree(pszSchemaName);
                return nullptr;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Handle the GEOM_TYPE option.                                    */
    /* -------------------------------------------------------------------- */
    pszGeomType = CSLFetchNameValue(papszOptions, "GEOM_TYPE");
    if (pszGeomType == nullptr)
    {
        pszGeomType = "geometry";
    }

    const char *pszGFldName = CSLFetchNameValue(papszOptions, "GEOMETRY_NAME");
    if (eType != wkbNone && EQUAL(pszGeomType, "geography"))
    {
        if (pszGFldName == nullptr)
            pszGFldName = "the_geog";
    }
    else if (eType != wkbNone && !EQUAL(pszGeomType, "geography"))
    {
        if (pszGFldName == nullptr)
            pszGFldName = "wkb_geometry";
    }

    if (eType != wkbNone && !EQUAL(pszGeomType, "geometry") &&
        !EQUAL(pszGeomType, "geography"))
    {
        if (bHaveGeography)
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GEOM_TYPE in DMGEO2 must be 'geometry' or 'geography'.\n"
                     "Creation of layer %s with GEOM_TYPE %s has failed.",
                     pszLayerName, pszGeomType);

        CPLFree(pszTableName);
        CPLFree(pszSchemaName);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Try to get the SRS Id of this spatial reference system,         */
    /*      adding tot the srs table if needed.                             */
    /* -------------------------------------------------------------------- */
    int nSRSId = nUndefinedSRID;

    if (poSRS != nullptr)
        nSRSId = FetchSRSId(poSRS);

    if (eType != wkbNone && EQUAL(pszGeomType, "geography"))
    {
        if (poSRS != nullptr && !poSRS->IsGeographic())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "geography type only supports geographic SRS");

            CPLFree(pszTableName);
            CPLFree(pszSchemaName);
            return nullptr;
        }

        if (nSRSId == nUndefinedSRID)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Assuming EPSG:4326 for geographic type (but no implicit "
                     "reprojection to it will be done)");
            nSRSId = 4326;
        }
    }

    const char *pszGeometryType = OGRToOGCGeomType(eType);

    const bool bFID64 = CPLFetchBool(papszOptions, "FID64", false);
    const char *pszSerialType = bFID64 ? "BIGINT" : "INT";

    CPLString osCreateTable;
    const bool bTemporary = CPLFetchBool(papszOptions, "TEMPORARY", false);
    if (bTemporary)
    {
        CPLFree(pszSchemaName);
        pszSchemaName = CPLStrdup("dm_temp_1");
        osCreateTable.Printf("CREATE global TEMPORARY TABLE %s", pszTableName);
    }
    else
    {
        osCreateTable.Printf("CREATE TABLE \"%s\".\"%s\"", pszSchemaName,
                             pszTableName);
    }

    const char *suffix = nullptr;
    if ((GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
        (GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
        suffix = "ZM";
    else if ((GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED) &&
             (EQUAL(pszGeomType, "geography") ||
              wkbFlatten(eType) != wkbUnknown))
        suffix = "M";
    else if (GeometryTypeFlags & OGRGeometry::OGR_G_3D)
        suffix = "Z";
    else
        suffix = "";

    if (eType != wkbNone && EQUAL(pszGeomType, "geography"))
    {
        osCommand.Printf("%s ( %s %s identity(1,1), %s SYSGEO2.ST_Geography "
                         "check(type=%s%s) check(srid = %d) , PRIMARY KEY (%s)",
                         osCreateTable.c_str(), osFIDColumnName.c_str(),
                         pszSerialType, pszGFldName, pszGeometryType, suffix,
                         nSRSId, osFIDColumnName.c_str());
    }
    else if (eType != wkbNone && !EQUAL(pszGeomType, "geography"))
    {
        osCommand.Printf("%s ( %s %s identity(1,1), %s SYSGEO2.ST_Geometry "
                         "check(type=%s%s) check(srid = %d) , PRIMARY KEY (%s)",
                         osCreateTable.c_str(), osFIDColumnName.c_str(),
                         pszSerialType, pszGFldName, pszGeometryType, suffix,
                         nSRSId, osFIDColumnName.c_str());
    }
    else
    {
        osCommand.Printf("%s ( %s %s identity(1,1), PRIMARY KEY (%s)",
                         osCreateTable.c_str(), osFIDColumnName.c_str(),
                         pszSerialType, osFIDColumnName.c_str());
    }
    osCreateTable = osCommand;

    bool bCreateSpatialIndex = FALSE;
    const char *pszSpatialIndexType = "SPGIST";
    osCommand = osCreateTable;
    osCommand += " )";

    OGRDMStatement oCommand(poSession);
    CPLErr rt = oCommand.Execute(osCommand.c_str());

    if (rt != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s\n", osCommand.c_str());
        CPLFree(pszTableName);
        CPLFree(pszSchemaName);

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    OGRDMTableLayer *poLayer = new OGRDMTableLayer(
        this, osCurrentSchema, pszTableName, pszSchemaName, "", nullptr, TRUE);
    poLayer->SetTableDefinition(osFIDColumnName, pszGFldName, eType,
                                pszGeomType, nSRSId, GeometryTypeFlags);
    poLayer->SetLaunderFlag(CPLFetchBool(papszOptions, "LAUNDER", true));
    poLayer->SetPrecisionFlag(CPLFetchBool(papszOptions, "PRECISION", true));
    //poLayer->SetForcedSRSId(nForcedSRSId);
    poLayer->SetForcedGeometryTypeFlags(ForcedGeometryTypeFlags);
    poLayer->SetCreateSpatialIndex(bCreateSpatialIndex, pszSpatialIndexType);
    poLayer->SetDeferredCreation(osCreateTable);

    /* HSTORE_COLUMNS existed at a time during GDAL 1.10dev */
    const char *pszHSTOREColumns =
        CSLFetchNameValue(papszOptions, "HSTORE_COLUMNS");
    if (pszHSTOREColumns != nullptr)
        CPLError(CE_Warning, CPLE_AppDefined,
                 "HSTORE_COLUMNS not recognized. Use COLUMN_TYPES instead.");

    const char *pszOverrideColumnTypes =
        CSLFetchNameValue(papszOptions, "COLUMN_TYPES");
    poLayer->SetOverrideColumnTypes(pszOverrideColumnTypes);

    if (bFID64)
        poLayer->SetMetadataItem(OLMD_FID64, "YES");

    /* -------------------------------------------------------------------- */
    /*      Add layer to data source layer list.                            */
    /* -------------------------------------------------------------------- */
    papoLayers = static_cast<OGRDMTableLayer **>(
        CPLRealloc(papoLayers, sizeof(OGRDMTableLayer *) * (nLayers + 1)));

    papoLayers[nLayers++] = poLayer;

    CPLFree(pszTableName);
    CPLFree(pszSchemaName);

    return poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRDMDataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer) || EQUAL(pszCap, ODsCDeleteLayer) ||
        EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCRandomLayerWrite))
        return TRUE;
    else
        return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRDMDataSource::GetLayer(int iLayer)

{
    /* Force loading of all registered tables */
    if (iLayer < 0 || iLayer >= GetLayerCount())
        return nullptr;
    else
        return papoLayers[iLayer];
}

/************************************************************************/
/*                           GetLayerByName()                           */
/************************************************************************/

OGRLayer *OGRDMDataSource::GetLayerByName(const char *pszNameIn)

{
    char *pszTableName = nullptr;
    char *pszGeomColumnName = nullptr;
    char *pszSchemaName = nullptr;

    if (!pszNameIn)
        return nullptr;

    /* first a case sensitive check */
    /* do NOT force loading of all registered tables */
    for (int i = 0; i < nLayers; i++)
    {
        OGRDMTableLayer *poLayer = papoLayers[i];

        if (strcmp(pszNameIn, poLayer->GetName()) == 0)
        {
            return poLayer;
        }
    }

    /* then case insensitive */
    for (int i = 0; i < nLayers; i++)
    {
        OGRDMTableLayer *poLayer = papoLayers[i];

        if (EQUAL(pszNameIn, poLayer->GetName()))
        {
            return poLayer;
        }
    }

    char *pszNameWithoutBracket = CPLStrdup(pszNameIn);
    char *pos = strchr(pszNameWithoutBracket, '(');
    if (pos != nullptr)
    {
        *pos = '\0';
        pszGeomColumnName = CPLStrdup(pos + 1);
        int len = static_cast<int>(strlen(pszGeomColumnName));
        if (len > 0)
            pszGeomColumnName[len - 1] = '\0';
    }

    pos = strchr(pszNameWithoutBracket, '.');
    if (pos != nullptr)
    {
        *pos = '\0';
        pszSchemaName = CPLStrdup(pszNameWithoutBracket);
        pszTableName = CPLStrdup(pos + 1);
    }
    else
    {
        pszTableName = CPLStrdup(pszNameWithoutBracket);
    }
    CPLFree(pszNameWithoutBracket);
    pszNameWithoutBracket = nullptr;

    OGRDMTableLayer *poLayer = nullptr;

    if (pszSchemaName != nullptr && osCurrentSchema == pszSchemaName &&
        pszGeomColumnName == nullptr)
    {
        poLayer =
            cpl::down_cast<OGRDMTableLayer *>(GetLayerByName(pszTableName));
    }
    else
    {
        CPLString osTableName(pszTableName);
        CPLString osTableNameLower(pszTableName);
        osTableNameLower.tolower();
        if (osTableName != osTableNameLower)
            CPLPushErrorHandler(CPLQuietErrorHandler);
        poLayer = OpenTable(osCurrentSchema, pszTableName, pszSchemaName,
                            nullptr, pszGeomColumnName, bDSUpdate, TRUE);
        if (osTableName != osTableNameLower)
            CPLPopErrorHandler();
        if (poLayer == nullptr && osTableName != osTableNameLower)
        {
            poLayer =
                OpenTable(osCurrentSchema, osTableNameLower, pszSchemaName,
                          nullptr, pszGeomColumnName, bDSUpdate, TRUE);
        }
    }

    CPLFree(pszTableName);
    CPLFree(pszSchemaName);
    CPLFree(pszGeomColumnName);

    return poLayer;
}

/************************************************************************/
/*                              FetchSRS()                              */
/*                                                                      */
/*      Return a SRS corresponding to a particular id.  Note that       */
/*      reference counting should be honoured on the returned           */
/*      OGRSpatialReference, as handles may be cached.                  */
/************************************************************************/

OGRSpatialReference *OGRDMDataSource::FetchSRS(int nId)

{
    if (nId < 0 || !m_bHasSpatialRefSys)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      First, we look through our SRID cache, is it there?             */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < nKnownSRID; i++)
    {
        if (panSRID[i] == nId)
            return papoSRS[i];
    }

    /* -------------------------------------------------------------------- */
    /*      Try looking up in spatial_ref_sys table.                        */
    /* -------------------------------------------------------------------- */
    CPLString osCommand;
    OGRSpatialReference *poSRS = nullptr;

    OGRDMStatement oCommand(poSession);
    osCommand.Printf(
        "SELECT srtext, auth_name, auth_srid FROM sysgeo2.spatial_ref_sys "
        "WHERE srid = %d",
        nId);
    CPLErr rt = oCommand.Execute(osCommand.c_str());

    if (rt == CE_None)
    {
        char **result = oCommand.SimpleFetchRow();
        if (result == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Could not fetch SRS");
        }
        else
        {
            const char *pszWKT = result[0];
            const char *pszAuthName = result[1];
            const char *pszAuthSRID = result[2];
            poSRS = new OGRSpatialReference();
            poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

            // Try to import first from EPSG code, and then from WKT
            if (pszAuthName && pszAuthSRID && EQUAL(pszAuthName, "EPSG") &&
                atoi(pszAuthSRID) == nId &&
                poSRS->importFromEPSG(nId) == OGRERR_NONE)
            {
                // do nothing
            }
            else if (poSRS->importFromWkt(pszWKT) != OGRERR_NONE)
            {
                delete poSRS;
                poSRS = nullptr;
            }
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Could not fetch SRS!");
    }

    if (poSRS)
        poSRS->StripTOWGS84IfKnownDatumAndAllowed();

    /* -------------------------------------------------------------------- */
    /*      Add to the cache.                                               */
    /* -------------------------------------------------------------------- */
    panSRID =
        static_cast<int *>(CPLRealloc(panSRID, sizeof(int) * (nKnownSRID + 1)));
    papoSRS = static_cast<OGRSpatialReference **>(
        CPLRealloc(papoSRS, sizeof(OGRSpatialReference *) * (nKnownSRID + 1)));
    panSRID[nKnownSRID] = nId;
    papoSRS[nKnownSRID] = poSRS;
    nKnownSRID++;

    return poSRS;
}

/************************************************************************/
/*                             FetchSRSId()                             */
/*                                                                      */
/*      Fetch the id corresponding to an SRS, and if not found, add     */
/*      it to the table.                                                */
/************************************************************************/

int OGRDMDataSource::FetchSRSId(const OGRSpatialReference *poSRS)

{
    if (poSRS == nullptr || !m_bHasSpatialRefSys)
        return nUndefinedSRID;

    OGRSpatialReference oSRS(*poSRS);
    // cppcheck-suppress uselessAssignmentPtrArg
    poSRS = nullptr;

    const char *pszAuthorityName = oSRS.GetAuthorityName(nullptr);

    if (pszAuthorityName == nullptr || strlen(pszAuthorityName) == 0)
    {
        /* -------------------------------------------------------------------- */
        /*      Try to identify an EPSG code                                    */
        /* -------------------------------------------------------------------- */
        oSRS.AutoIdentifyEPSG();

        pszAuthorityName = oSRS.GetAuthorityName(nullptr);
        if (pszAuthorityName != nullptr && EQUAL(pszAuthorityName, "EPSG"))
        {
            const char *pszAuthorityCode = oSRS.GetAuthorityCode(nullptr);
            if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
            {
                /* Import 'clean' SRS */
                oSRS.importFromEPSG(atoi(pszAuthorityCode));

                pszAuthorityName = oSRS.GetAuthorityName(nullptr);
            }
        }
    }
    /* -------------------------------------------------------------------- */
    /*      Check whether the authority name/code is already mapped to a    */
    /*      SRS ID.                                                         */
    /* -------------------------------------------------------------------- */
    CPLString osCommand;
    OGRDMStatement oCommand(poSession);
    CPLErr rt;
    int nAuthorityCode = 0;
    if (pszAuthorityName != nullptr)
    {
        /* Check that the authority code is integral */
        nAuthorityCode = atoi(oSRS.GetAuthorityCode(nullptr));
        if (nAuthorityCode > 0)
        {
            osCommand.Printf("SELECT srid FROM sysgeo2.spatial_ref_sys WHERE "
                             "auth_name = '%s' AND auth_srid = %d",
                             pszAuthorityName, nAuthorityCode);
            rt = oCommand.Execute(osCommand.c_str());
            char **result = nullptr;
            if (rt == CE_None)
                result = oCommand.SimpleFetchRow();

            if (result && rt == CE_None)
            {
                int nSRSId = atoi(result[0]);

                return nSRSId;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Translate SRS to WKT.                                           */
    /* -------------------------------------------------------------------- */
    char *pszWKT = nullptr;
    if (oSRS.exportToWkt(&pszWKT) != OGRERR_NONE)
    {
        CPLFree(pszWKT);
        return nUndefinedSRID;
    }

    /* -------------------------------------------------------------------- */
    /*      Try to find in the existing table.                              */
    /* -------------------------------------------------------------------- */
    osCommand.Printf(
        "SELECT srid FROM sysgeo2.spatial_ref_sys WHERE srtext = '%s'", pszWKT);
    rt = oCommand.Execute(osCommand.c_str());

    /* -------------------------------------------------------------------- */
    /*      We got it!  Return it.                                          */
    /* -------------------------------------------------------------------- */
    char **result = nullptr;
    if (rt == CE_None)
        result = oCommand.SimpleFetchRow();
    if (result && rt == CE_None)
    {
        int nSRSId = atoi(result[0]);

        return nSRSId;
    }

    /* -------------------------------------------------------------------- */
    /*      If the command actually failed, then the metadata table is      */
    /*      likely missing. Try defining it.                                */
    /* -------------------------------------------------------------------- */
    const bool bTableMissing = result == nullptr || rt != CE_None;

    if (bTableMissing)
    {
        return nUndefinedSRID;
    }

    /* -------------------------------------------------------------------- */
    /*      Get the current maximum srid in the srs table.                  */
    /* -------------------------------------------------------------------- */

    osCommand.Printf("SELECT MAX(srid) FROM sysgeo2.spatial_ref_sys");
    rt = oCommand.Execute(osCommand.c_str());
    int nSRSId = 1;
    result = nullptr;
    if (rt == CE_None)
        result = oCommand.SimpleFetchRow();
    if (result && rt == CE_None)
    {
        nSRSId = atoi(result[0]) + 1;
    }

    /* -------------------------------------------------------------------- */
    /*      Try adding the SRS to the SRS table.                            */
    /* -------------------------------------------------------------------- */
    char *pszProj4 = nullptr;
    if (oSRS.exportToProj4(&pszProj4) != OGRERR_NONE)
    {
        CPLFree(pszProj4);
        return nUndefinedSRID;
    }

    if (pszAuthorityName != nullptr && nAuthorityCode > 0)
    {
        nAuthorityCode = atoi(oSRS.GetAuthorityCode(nullptr));

        osCommand.Printf("INSERT INTO sysgeo2.spatial_ref_sys "
                         "(srid,srtext,proj4text,auth_name,auth_srid) "
                         "VALUES (%d, %s, %s, '%s', %d)",
                         nSRSId, pszWKT, pszProj4, pszAuthorityName,
                         nAuthorityCode);
    }
    else
    {
        osCommand.Printf("INSERT INTO sysgeo2.spatial_ref_sys "
                         "(srid,srtext,proj4text) VALUES (%d,%s,%s)",
                         nSRSId, pszWKT, pszProj4);
    }

    // Free everything that was allocated.
    CPLFree(pszProj4);
    CPLFree(pszWKT);
    pszWKT = nullptr;  // CM:  Added

    rt = oCommand.Execute(osCommand.c_str());

    return nSRSId;
}

/************************************************************************/
/*                           GetMetadataItem()                          */
/************************************************************************/

const char *OGRDMDataSource::GetMetadataItem(const char *pszKey,
                                             const char *pszDomain)
{
    /* Only used by ogr_dm.py to check inner working */
    if (pszDomain != nullptr && EQUAL(pszDomain, "_debug_") &&
        pszKey != nullptr)
    {
        if (EQUAL(pszKey, "bHasLoadTables"))
            return CPLSPrintf("%d", bHasLoadTables);
    }
    return OGRDataSource::GetMetadataItem(pszKey, pszDomain);
}

/************************************************************************/
/*                             ExecuteSQL()                             */
/************************************************************************/

OGRLayer *OGRDMDataSource::ExecuteSQL(const char *pszSQLCommand,
                                      OGRGeometry *poSpatialFilter,
                                      const char *pszDialect)

{
    /* Skip leading whitespace characters */
    while (std::isspace(static_cast<unsigned char>(*pszSQLCommand)))
        pszSQLCommand++;

    /* -------------------------------------------------------------------- */
    /*      Use generic implementation for recognized dialects              */
    /* -------------------------------------------------------------------- */
    if (IsGenericSQLDialect(pszDialect))
        return OGRDataSource::ExecuteSQL(pszSQLCommand, poSpatialFilter,
                                         pszDialect);

    /* -------------------------------------------------------------------- */
    /*      Special case DELLAYER: command.                                 */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszSQLCommand, "DELLAYER:"))
    {
        const char *pszLayerName = pszSQLCommand + 9;

        while (*pszLayerName == ' ')
            pszLayerName++;

        GetLayerCount();
        for (int iLayer = 0; iLayer < nLayers; iLayer++)
        {
            if (EQUAL(papoLayers[iLayer]->GetName(), pszLayerName))
            {
                DeleteLayer(iLayer);
                break;
            }
        }
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Execute the statement.                                          */
    /* -------------------------------------------------------------------- */
    OGRDMStatement oCommand(poSession);

    if (STARTS_WITH_CI(pszSQLCommand, "SELECT") == FALSE ||
        (strstr(pszSQLCommand, "from") == nullptr &&
         strstr(pszSQLCommand, "FROM") == nullptr))
    {
        /* For something that is not a select or a select without table, do not */
        /* run under transaction (CREATE DATABASE, VACUUM don't like transactions) */
        CPLErr rt = oCommand.Execute(pszSQLCommand);
        if (rt == CE_None)
        {
            CPLDebug("DM", "Error Results Tuples");
            return nullptr;
        }
        return nullptr;
    }
    else
    {
        /* -------------------------------------------------------------------- */
        /*      Do we have a tuple result? If so, instantiate a results         */
        /*      layer for it.                                                   */
        /* -------------------------------------------------------------------- */
        if (oCommand.Execute(pszSQLCommand) == CE_None)
        {
            OGRDMResultLayer *poLayer =
                new OGRDMResultLayer(this, pszSQLCommand, &oCommand);

            if (poSpatialFilter != nullptr)
                poLayer->SetSpatialFilter(poSpatialFilter);

            return poLayer;
        }
    }
    return nullptr;
}

/************************************************************************/
/*                          ReleaseResultSet()                          */
/************************************************************************/

void OGRDMDataSource::ReleaseResultSet(OGRLayer *poLayer)

{
    delete poLayer;
}
