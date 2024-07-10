/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRDMTableLayer class.
 * Author:   Yilun Wu, wuyilun@dameng.com
 *
 ******************************************************************************
 * Copyright (c) 2024, Yilun Wu (wuyilun@dameng.com)
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
#include <ogr_p.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

CPL_CVSID("$Id$")

/************************************************************************/
/*                        OGRDMTableFeatureDefn                         */
/************************************************************************/

class OGRDMTableFeatureDefn final : public OGRDMFeatureDefn
{
  private:
    OGRDMTableFeatureDefn(const OGRDMTableFeatureDefn &) = delete;
    OGRDMTableFeatureDefn &operator=(const OGRDMTableFeatureDefn &) = delete;

    OGRDMTableLayer *poLayer = nullptr;

    void SolveFields() const;

  public:
    explicit OGRDMTableFeatureDefn(OGRDMTableLayer *poLayerIn,
                                   const char *pszName = nullptr)
        : OGRDMFeatureDefn(pszName), poLayer(poLayerIn)
    {
    }

    virtual int GetFieldCount() const override
    {
        SolveFields();
        return OGRDMFeatureDefn::GetFieldCount();
    }
    virtual OGRFieldDefn *GetFieldDefn(int i) override
    {
        SolveFields();
        return OGRDMFeatureDefn::GetFieldDefn(i);
    }
    virtual const OGRFieldDefn *GetFieldDefn(int i) const override
    {
        SolveFields();
        return OGRDMFeatureDefn::GetFieldDefn(i);
    }
    virtual int GetGeomFieldCount() const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRDMFeatureDefn::GetGeomFieldCount();
    }
    virtual OGRDMGeomFieldDefn *GetGeomFieldDefn(int i) override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRDMFeatureDefn::GetGeomFieldDefn(i);
    }
    virtual const OGRDMGeomFieldDefn *GetGeomFieldDefn(int i) const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRDMFeatureDefn::GetGeomFieldDefn(i);
    }
    virtual int GetGeomFieldIndex(const char *pszName) const override
    {
        if (poLayer != nullptr && !poLayer->HasGeometryInformation())
            SolveFields();
        return OGRDMFeatureDefn::GetGeomFieldIndex(pszName);
    }
};

void OGRDMTableFeatureDefn::SolveFields() const

{
    if (poLayer == nullptr)
        return;

    poLayer->ReadTableDefinition();
}

/************************************************************************/
/*                            GetFIDColumn()                            */
/************************************************************************/

const char *OGRDMTableLayer::GetFIDColumn()

{
    ReadTableDefinition();

    if (pszFIDColumn != nullptr)
        return pszFIDColumn;
    else
        return "";
}

OGRDMTableLayer::OGRDMTableLayer(OGRDMDataSource *poDSIn,
                                 CPLString &osCurrentSchema,
                                 const char *pszTableNameIn,
                                 const char *pszSchemaNameIn,
                                 const char *pszDescriptionIn,
                                 const char *pszGeomColForcedIn,
                                 int bUpdateIn)
    : bUpdate(bUpdateIn), pszTableName(CPLStrdup(pszTableNameIn)),
      pszSchemaName(CPLStrdup(pszSchemaNameIn ? pszSchemaNameIn
                                              : osCurrentSchema.c_str())),
      m_pszTableDescription(pszDescriptionIn ? CPLStrdup(pszDescriptionIn)
                                             : nullptr),
      pszGeomColForced(pszGeomColForcedIn ? CPLStrdup(pszGeomColForcedIn)
                                          : nullptr)
{
    poDS = poDSIn;
    pszQueryStatement = nullptr;

    CPLString osDefnName;
    if (pszSchemaNameIn && osCurrentSchema != pszSchemaNameIn)
    {
        osDefnName.Printf("%s.%s", pszSchemaNameIn, pszTableName);
        pszSqlTableName = CPLStrdup(
            CPLString().Printf("%s.%s", pszSchemaNameIn, pszTableName));
    }
    else
    {
        osDefnName = pszTableName;
        pszSqlTableName = CPLStrdup(pszTableName);
    }
    if (pszGeomColForced != nullptr)
    {
        osDefnName += "(";             
        osDefnName += pszGeomColForced;
        osDefnName += ")";             
    }

    poFeatureDefn = new OGRDMTableFeatureDefn(this, osDefnName);
    SetDescription(poFeatureDefn->GetName());
    poFeatureDefn->Reference();

    if (pszDescriptionIn != nullptr && !EQUAL(pszDescriptionIn, ""))
    {
        OGRLayer::SetMetadataItem("DESCRIPTION", pszDescriptionIn);
    }
}

/************************************************************************/
/*                          ~OGRDMTableLayer()                          */
/************************************************************************/

OGRDMTableLayer::~OGRDMTableLayer()

{
    CPLFree(pszSqlTableName);
    CPLFree(pszTableName);
    CPLFree(pszSqlGeomParentTableName);
    CPLFree(pszSchemaName);
    CPLFree(m_pszTableDescription);
    CPLFree(pszGeomColForced);
    if (InsertStatement)
        delete InsertStatement;
    CSLDestroy(papszOverrideColumnTypes);
}

/************************************************************************/
/*                          GetMetadataDomainList()                     */
/************************************************************************/

char **OGRDMTableLayer::GetMetadataDomainList()
{
    if (m_pszTableDescription == nullptr)
        GetMetadata();
    if (m_pszTableDescription != nullptr && m_pszTableDescription[0] != '\0')
        return CSLAddString(nullptr, "");
    return nullptr;
}

/************************************************************************/
/*                              GetMetadata()                           */
/************************************************************************/

char **OGRDMTableLayer::GetMetadata(const char *pszDomain)
{
    if ((pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        m_pszTableDescription == nullptr)
    {
        OGRDMConn *hDMConn = poDS->GetDMConn();
        OGRDMStatement oCommand(hDMConn);
        CPLString osCommand;
        osCommand.Printf("SELECT COMMENTS FROM ALL_TAB_COMMENTS "
                         "WHERE TABLE_NAME = '%s' AND OWNER = '%s'",
                         pszTableName, pszSchemaName);
        CPLErr eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLDebug("DM", "Could not Get Metadata of the table %s",
                     pszTableName);
            return nullptr;        
        }

        const char *pszDesc = nullptr;
        char **hResult = oCommand.SimpleFetchRow();
        if (hResult)
        {
            pszDesc = hResult[0];
            if (pszDesc)
                OGRLayer::SetMetadataItem("DESCRIPTION", pszDesc);
        }
        m_pszTableDescription = CPLStrdup(pszDesc ? pszDesc : "");
    }

    return OGRLayer::GetMetadata(pszDomain);
}

/************************************************************************/
/*                            GetMetadataItem()                         */
/************************************************************************/

const char *OGRDMTableLayer::GetMetadataItem(const char *pszName,
                                             const char *pszDomain)
{
    GetMetadata(pszDomain);
    return OGRLayer::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                              SetMetadata()                           */
/************************************************************************/

CPLErr OGRDMTableLayer::SetMetadata(char **papszMD,
                                    const char *pszDomain)
{
    OGRLayer::SetMetadata(papszMD, pszDomain);
    if (!osForcedDescription.empty() &&
        (pszDomain == nullptr || EQUAL(pszDomain, "")))
    {
        OGRLayer::SetMetadataItem("DESCRIPTION", osForcedDescription);
    }

    if (!bDeferredCreation && (pszDomain == nullptr || EQUAL(pszDomain, "")))
    {
        const char *pszDescription = OGRLayer::GetMetadataItem("DESCRIPTION");
        if (pszDescription == nullptr)
            pszDescription = "";
        OGRDMConn *hDMConn = poDS->GetDMConn();
        OGRDMStatement oCommand(hDMConn);
        CPLString osCommand;

        osCommand.Printf("COMMENT ON TABLE \"%s\" IS %s", pszSqlTableName,
                         pszDescription[0] != '\0' ? pszDescription : "''");
        CPLErr eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLDebug("DM", "Could not Set Metadata of the table %s",
                     pszTableName);
            return CE_Warning;     
        }
        CPLFree(m_pszTableDescription);
        m_pszTableDescription = CPLStrdup(pszDescription);
    }

    return CE_None;
}

/************************************************************************/
/*                            SetMetadataItem()                         */
/************************************************************************/

CPLErr OGRDMTableLayer::SetMetadataItem(const char *pszName,
                                        const char *pszValue,
                                        const char *pszDomain)
{
    if ((pszDomain == nullptr || EQUAL(pszDomain, "")) && pszName != nullptr &&
        EQUAL(pszName, "DESCRIPTION") && !osForcedDescription.empty())
    {
        pszValue = osForcedDescription;
    }
    OGRLayer::SetMetadataItem(pszName, pszValue, pszDomain);
    if (!bDeferredCreation && (pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        pszName != nullptr && EQUAL(pszName, "DESCRIPTION"))
    {
        SetMetadata(GetMetadata());
    }
    return CE_None;
}

/************************************************************************/
/*                      SetForcedDescription()                          */
/************************************************************************/

void OGRDMTableLayer::SetForcedDescription(const char *pszDescriptionIn)
{
    osForcedDescription = pszDescriptionIn;
    CPLFree(m_pszTableDescription);
    m_pszTableDescription = CPLStrdup(pszDescriptionIn);
    SetMetadataItem("DESCRIPTION", osForcedDescription);
}

/************************************************************************/
/*                      SetGeometryInformation()                        */
/************************************************************************/

void OGRDMTableLayer::SetGeometryInformation(DMGeomColumnDesc *pasDesc,
                                             int nGeomFieldCount)
{
    for (int i = 0; i < nGeomFieldCount; i++)
    {
        auto poGeomFieldDefn =
            std::make_unique<OGRDMGeomFieldDefn>(this, pasDesc[i].pszName);
        poGeomFieldDefn->SetNullable(pasDesc[i].bNullable);
        poGeomFieldDefn->nSRSId = pasDesc[i].nSRID;
        poGeomFieldDefn->GeometryTypeFlags = pasDesc[i].GeometryTypeFlags;
        poGeomFieldDefn->eDMGeoType = pasDesc[i].eDMGeoType;
        if (pasDesc[i].pszGeomType != nullptr)
        {
            OGRwkbGeometryType eGeomType =
                OGRFromOGCGeomType(pasDesc[i].pszGeomType);
            if ((poGeomFieldDefn->GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
                (eGeomType != wkbUnknown))
                eGeomType = wkbSetZ(eGeomType);
            if ((poGeomFieldDefn->GeometryTypeFlags &
                 OGRGeometry::OGR_G_MEASURED) &&
                (eGeomType != wkbUnknown))
                eGeomType = wkbSetM(eGeomType);
            poGeomFieldDefn->SetType(eGeomType);
        }
        poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    }
}

/************************************************************************/
/*                        ReadTableDefinition()                         */
/*                                                                      */
/*      Build a schema from the named table.  Done by querying the      */
/*      catalog.                                                        */
/************************************************************************/

int OGRDMTableLayer::ReadTableDefinition()

{
    OGRDMConn *hDMConn = poDS->GetDMConn();

    if (bTableDefinitionValid >= 0)
        return bTableDefinitionValid;
    bTableDefinitionValid = FALSE;

    /* -------------------------------------------------------------------- */
    /*      Get the OID of the table.                                       */
    /* -------------------------------------------------------------------- */

    CPLString osCommand;
    OGRDMStatement oCommand(hDMConn);
    osCommand.Printf("SELECT o.id FROM SYSOBJECTS o "
                     "JOIN DBA_TABLES t ON o.NAME=t.TABLE_NAME "
                     "WHERE o.NAME = '%s' AND t.OWNER = '%s'",
                     pszTableName, pszSchemaName);
    CPLErr eErr = oCommand.Execute(osCommand);
    if (eErr != CE_None)
    {
        CPLDebug("DM", "Could not Get the OID of the table %s",
                 pszTableName);
        return FALSE;          
    }
    unsigned int nTableOID = 0;
    char **hResult = oCommand.SimpleFetchRow();

    if (hResult)
    {
        if (hResult[0])
        {
            nTableOID = static_cast<unsigned>(CPLAtoGIntBig(hResult[0]));
            if (oCommand.SimpleFetchRow())
            {
                CPLDebug("DM", "Could not retrieve table oid for %s",
                         pszTableName);
                return FALSE;          
            }
        }
    }
    else
    {
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Identify the integer primary key.                               */
    /* -------------------------------------------------------------------- */

    const char *pszTypnameEqualAnyClause =
        "ANY('INT', 'BIGINT', 'SMALLINT', 'INTEGER')";

    osCommand.Printf("SELECT c.NAME, c.COLID, c.TYPE$ "
                     "FROM SYSCOLUMNS c "
                     "JOIN SYSOBJECTS o ON o.ID = c.ID "
                     "WHERE c.ID = %u AND c.TYPE$ = %s "
                     "ORDER BY c.COLID",
                     nTableOID, pszTypnameEqualAnyClause);

    eErr = oCommand.Execute(osCommand);
    hResult = oCommand.SimpleFetchRow();

    if (hResult)
    {
        if (hResult[0])
        {
            osPrimaryKey.Printf("%s", hResult[0]);
            const char *pszFIDType = hResult[2];
            CPLDebug("DM", "Primary key name (FID): %s, type : %s",
                     osPrimaryKey.c_str(), pszFIDType);
            if (oCommand.SimpleFetchRow())
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Multi-column primary key in \'%s\' detected but not "
                         "supported.",
                         pszTableName);
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Identify the integer primary key Failed");
    }

    /* -------------------------------------------------------------------- */
    /*      Fire off commands to get back the columns of the table.         */
    /* -------------------------------------------------------------------- */
    osCommand.Printf(
        "SELECT c.NAME, c.TYPE$, c.NULLABLE$, c.DEFVAL, i.UNIQUENESS "
        "FROM SYSCOLUMNS c "
        "JOIN SYSOBJECTS o ON o.ID = c.ID "
        "LEFT JOIN "
        "USER_INDEXES i ON i.TABLE_NAME = c.NAME "
        "WHERE c.ID = %u "
        "ORDER BY c.COLID",
        nTableOID);

    eErr = oCommand.Execute(osCommand);
    hResult = oCommand.SimpleFetchRow();
    if (!hResult || eErr != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Fire off commands to get back the columns of the table "
                 "Failed!");         
        return bTableDefinitionValid;
    }

    /* -------------------------------------------------------------------- */
    /*      Parse the returned table information.                           */
    /* -------------------------------------------------------------------- */
    int typid = 0;
    for (int iRecord = 0; hResult; iRecord++)
    {
        OGRFieldDefn oField(hResult[0], OFTString);
        const char *type = hResult[1];
        typid = atoi(type + 5);
        const char *pszType;
        if (typid >= NDCT_CLSID_GEO2_ST_GEOMETRY &&
            typid <= NDCT_CLSID_GEO2_ST_TIN)
            pszType = "geometry";
        else if (typid == NDCT_CLSID_GEO2_ST_GEOGRAPHY)
            pszType = "geography";
        else
            pszType = type;
        const char *pszNotNull = hResult[2];
        const char *pszDefault = hResult[3];
        const char *pszIsUnique = hResult[4] ? nullptr : hResult[4];
        if (pszNotNull && EQUAL(pszNotNull, "Y"))
            oField.SetNullable(FALSE);
        if (pszIsUnique && EQUAL(pszIsUnique, "UNIQUE"))
            oField.SetUnique(TRUE);

        if (EQUAL(oField.GetNameRef(), osPrimaryKey))
        {
            pszFIDColumn = CPLStrdup(oField.GetNameRef());
            CPLDebug("DM", "Using column '%s' as FID for table '%s'",
                     pszFIDColumn, pszTableName);
            hResult = oCommand.SimpleFetchRow();
            continue;
        }
        else if (EQUAL(pszType, "geometry") || EQUAL(pszType, "geography"))
        {
            const auto InitGeomField =
                [this, &pszType, &oField](OGRDMGeomFieldDefn *poGeomFieldDefn)
            {
                if (EQUAL(pszType, "geometry"))
                    poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOMETRY;
                else if (EQUAL(pszType, "geography"))
                {
                    poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOGRAPHY;
                    poGeomFieldDefn->nSRSId = 4326;                   
                }
                poGeomFieldDefn->SetNullable(oField.IsNullable());
            };
            if (!bGeometryInformationSet)
            {
                if (pszGeomColForced == nullptr ||
                    EQUAL(pszGeomColForced, oField.GetNameRef()))
                {
                    auto poGeomFieldDefn = std::make_unique<OGRDMGeomFieldDefn>(
                        this, oField.GetNameRef());
                    InitGeomField(poGeomFieldDefn.get());
                    poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
                }
            }
            else
            {
                int idx =
                    poFeatureDefn->GetGeomFieldIndex(oField.GetNameRef());
                if (idx >= 0)                                             
                {
                    auto poGeomFieldDefn =
                        poFeatureDefn->GetGeomFieldDefn(idx);
                    InitGeomField(poGeomFieldDefn);          
                }
            }
            hResult = oCommand.SimpleFetchRow();
            continue;
        }

        /*  OGRDMCommonLayerSetType(oField, pszType, scale, nWidth);
      */
        if (pszDefault)
            oField.SetDefault(pszDefault);

        poFeatureDefn->AddFieldDefn(&oField);
        hResult = oCommand.SimpleFetchRow();
    }
    bTableDefinitionValid = TRUE;

    ResetReading();

    /* If geometry type, SRID, etc... have always been set by SetGeometryInformation() */
    /* no need to issue a new SQL query. Just record the geom type in the layer definition */
    if (bGeometryInformationSet)
    {
        return TRUE;
    }
    bGeometryInformationSet = TRUE;

    // get layer geometry type
    for (int iField = 0; iField < poFeatureDefn->GetGeomFieldCount(); iField++)
    {
        OGRDMGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(iField);

        /* Get the geometry type and dimensions from the table, or */
        /* from its parents if it is a derived table, or from the parent of the parent, etc.. */
        int bGoOn = poDS->m_bHasGeometryColumns;
        const bool bHasGeometry =
            (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY);

        while (bGoOn)
        {
            eErr = CheckINI(&checkINI);
            osCommand.Printf("SELECT type, coord_dimension, srid FROM %s WHERE "
                             "f_table_name = '%s'",
                             (bHasGeometry) ? "sysgeo2.geometry_columns"
                                            : "sysgeo2.geography_columns",
                             pszTableName);

            osCommand += CPLString().Printf(
                " AND %s='%s'",
                (bHasGeometry) ? "f_geometry_column" : "f_geography_column",
                poGeomFieldDefn->GetNameRef());

            osCommand +=
                CPLString().Printf(" AND f_table_schema = '%s'", pszSchemaName);

            eErr = oCommand.Execute(osCommand);
            hResult = oCommand.SimpleFetchRow();

            if (hResult && hResult[0] && !oCommand.SimpleFetchRow())
            {
                const char *pszType = hResult[0] + 3;
                int dim = atoi(hResult[1]);
                bool bHasM = pszType[strlen(pszType) - 1] == 'M';
                int GeometryTypeFlags = 0;
                if (dim == 3)
                {
                    if (bHasM)                                           
                        GeometryTypeFlags |= OGRGeometry::OGR_G_MEASURED;
                    else
                        GeometryTypeFlags |= OGRGeometry::OGR_G_3D;
                }
                else if (dim == 4)
                    GeometryTypeFlags |=
                        OGRGeometry::OGR_G_3D | OGRGeometry::OGR_G_MEASURED;

                int nSRSId = atoi(hResult[2]);

                poGeomFieldDefn->GeometryTypeFlags = GeometryTypeFlags;
                if (nSRSId > 0)
                    poGeomFieldDefn->nSRSId = nSRSId;
                OGRwkbGeometryType eGeomType;
                if (checkINI)
                    eGeomType = OGRFromOGCGeomType(pszType);
                else
                    eGeomType = OGRDMCheckType(typid);
                if (poGeomFieldDefn->GeometryTypeFlags &
                        OGRGeometry::OGR_G_3D &&
                    eGeomType != wkbUnknown)
                    eGeomType = wkbSetZ(eGeomType);
                if (poGeomFieldDefn->GeometryTypeFlags &
                        OGRGeometry::OGR_G_MEASURED &&
                    eGeomType != wkbUnknown)
                    eGeomType = wkbSetM(eGeomType);
                poGeomFieldDefn->SetType(eGeomType);

                bGoOn = FALSE;
            }
            else
            {
                /* Fetch the name of the parent table */
                osCommand.Printf("SELECT o.NAME FROM SYSOBJECTS o WHERE ID = "
                                 "(SELECT o.PID FROM SYSOBJECTS o "
                                 "JOIN DBA_TABLES t ON c.NAME=t.TABLE_NAME "
                                 "WHERE o.NAME = '%s' AND t.OWNER = '%s') ",
                                 pszTableName,
                                 pszSchemaName);

                eErr = oCommand.Execute(osCommand); 
                hResult = oCommand.SimpleFetchRow();

                if (hResult && hResult[0] && !oCommand.SimpleFetchRow())
                {
                    CPLFree(pszSqlGeomParentTableName);               
                    pszSqlGeomParentTableName = CPLStrdup(hResult[0]);
                }
                else
                {
                    /* No more parent : stop recursion */
                    bGoOn = FALSE;
                }
            }
        }
    }

    return bTableDefinitionValid;
}

/************************************************************************/
/*                         SetTableDefinition()                         */
/************************************************************************/

void OGRDMTableLayer::SetTableDefinition(const char *pszFIDColumnName,
                                         const char *pszGFldName,
                                         OGRwkbGeometryType eType,
                                         const char *pszGeomType,
                                         int nSRSId,
                                         int GeometryTypeFlags)
{
    bTableDefinitionValid = TRUE;
    bGeometryInformationSet = TRUE;
    pszFIDColumn = CPLStrdup(pszFIDColumnName);
    poFeatureDefn->SetGeomType(wkbNone);
    if (eType != wkbNone)
    {
        auto poGeomFieldDefn =
            std::make_unique<OGRDMGeomFieldDefn>(this, pszGFldName);
        poGeomFieldDefn->SetType(eType);
        poGeomFieldDefn->GeometryTypeFlags = GeometryTypeFlags;

        if (EQUAL(pszGeomType, "geometry"))
        {
            poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOMETRY;
            poGeomFieldDefn->nSRSId = nSRSId;
        }
        else if (EQUAL(pszGeomType, "geography"))
        {
            poGeomFieldDefn->eDMGeoType = GEOM_TYPE_GEOGRAPHY;
            poGeomFieldDefn->nSRSId = nSRSId;                 
        }
        poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    }
    else if (pszGFldName != nullptr)
    {
        m_osFirstGeometryFieldName = pszGFldName;
    }
}

/************************************************************************/
/*                          SetSpatialFilter()                          */
/************************************************************************/

void OGRDMTableLayer::SetSpatialFilter(int iGeomField,
                                       OGRGeometry *poGeomIn)

{
    if (iGeomField < 0 || iGeomField >= GetLayerDefn()->GetGeomFieldCount() ||
        GetLayerDefn()->GetGeomFieldDefn(iGeomField)->GetType() == wkbNone)
    {
        if (iGeomField != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid geometry field index : %d", iGeomField);
        }
        return;
    }
    m_iGeomFieldFilter = iGeomField;

    if (InstallFilter(poGeomIn))
    {
        BuildWhere();

        ResetReading();
    }
}

/************************************************************************/
/*                             BuildWhere()                             */
/*                                                                      */
/*      Build the WHERE statement appropriate to the current set of     */
/*      criteria (spatial and attribute queries).                       */
/************************************************************************/

void OGRDMTableLayer::BuildWhere()

{
    osWHERE = "";                                 
    OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;
    if (poFeatureDefn->GetGeomFieldCount() != 0)  
        poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);

    if (m_poFilterGeom != nullptr && poGeomFieldDefn != nullptr &&
        poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY)
    {
        char szBox3D_1[128];
        char szBox3D_2[128];
        char szBox3D_3[128];
        char szBox3D_4[128];
        OGREnvelope sEnvelope;

        m_poFilterGeom->getEnvelope(&sEnvelope);
        CPLsnprintf(szBox3D_1, sizeof(szBox3D_1), "%.18g %.18g", sEnvelope.MinX,
                    sEnvelope.MinY);
        CPLsnprintf(szBox3D_2, sizeof(szBox3D_2), "%.18g %.18g", sEnvelope.MinX,
                    sEnvelope.MaxY);
        CPLsnprintf(szBox3D_3, sizeof(szBox3D_2), "%.18g %.18g", sEnvelope.MaxX,
                    sEnvelope.MaxY);
        CPLsnprintf(szBox3D_4, sizeof(szBox3D_2), "%.18g %.18g", sEnvelope.MaxX,
                    sEnvelope.MinY);
        osWHERE.Printf(
            "WHERE DMGEO2.ST_BOXCONTAINS(dmgeo2.st_geogfromtext('POLYGON(( %s, "
            "%s, %s, %s, %s))'), %s);",
            szBox3D_1, szBox3D_2, szBox3D_3, szBox3D_4, szBox3D_1,
            poGeomFieldDefn->GetNameRef());
    }
    else
    {
        osWHERE = "";
    }
}

/************************************************************************/
/*                      BuildFullQueryStatement()                       */
/************************************************************************/

void OGRDMTableLayer::BuildFullQueryStatement()

{
    CPLString osFields = BuildFields();
    if (pszQueryStatement != nullptr)
    {
        CPLFree(pszQueryStatement); 
        pszQueryStatement = nullptr;
    }
    pszQueryStatement = static_cast<char *>(CPLMalloc(
        osFields.size() + osWHERE.size() + strlen(pszSqlTableName) + 40));
    snprintf(pszQueryStatement,
             osFields.size() + osWHERE.size() + strlen(pszSqlTableName) + 40,
             "SELECT %s FROM \"%s\" %s", osFields.c_str(), pszSqlTableName,
             osWHERE.c_str());
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRDMTableLayer::ResetReading()

{
    if (bInResetReading)
        return;
    bInResetReading = TRUE;

    BuildFullQueryStatement();

    OGRDMLayer::ResetReading();

    bInResetReading = FALSE;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRDMTableLayer::GetNextFeature()

{
    if (pszQueryStatement == nullptr)
        ResetReading();              

    OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;
    if (poFeatureDefn->GetGeomFieldCount() != 0)  
        poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
    poFeatureDefn->GetFieldCount();                             

    while (true)
    {
        OGRFeature *poFeature = GetNextRawFeature();
        if (poFeature == nullptr)                   
            return nullptr;                         

        /* We just have to look if there is a geometry filter */
        /* If there's a geometry column, the spatial filter */
        /* is already taken into account in the select request */
        /* The attribute filter is always taken into account by the select request */
        if (m_poFilterGeom == nullptr || poGeomFieldDefn == nullptr ||
            poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY ||
            poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY ||
            FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter)))
        {
            if (iFIDAsRegularColumnIndex >= 0)
            {
                poFeature->SetField(iFIDAsRegularColumnIndex,
                                    poFeature->GetFID());
            }
            return poFeature;
        }

        delete poFeature;
    }
}

/************************************************************************/
/*                            BuildFields()                             */
/*                                                                      */
/*      Build list of fields to fetch, performing any required          */
/*      transformations (such as on geometry).                          */
/************************************************************************/

CPLString OGRDMTableLayer::BuildFields()

{
    int i = 0;
    CPLString osFieldList;

    poFeatureDefn->GetFieldCount();

    if (pszFIDColumn != nullptr &&
        poFeatureDefn->GetFieldIndex(pszFIDColumn) == -1)
    {
        osFieldList += pszFIDColumn;
    }

    for (i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRDMGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(i);
        CPLString osEscapedGeom = poGeomFieldDefn->GetNameRef();

        if (!osFieldList.empty())
            osFieldList += ", ";

        if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY)
        {
            if (poDS->bUseBinaryCursor)
            {
                osFieldList += osEscapedGeom;
            }
            else if (!CPLTestBool(CPLGetConfigOption("DM_USE_TEXT", "NO")))
            {
                /* This will return EWKB in an hex encoded form */
                osFieldList += osEscapedGeom;
            }
            /* else
            {
                osFieldList += "DMGEO2.ST_AsEWKT(";
                osFieldList += osEscapedGeom;
                osFieldList += ") AS ";
                osFieldList += CPLSPrintf("AsEWKT_%s", poGeomFieldDefn->GetNameRef());
            }*/
        }
        else if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY)
        {

            if (!CPLTestBool(CPLGetConfigOption("DM_USE_TEXT", "NO")))
            {
                osFieldList += osEscapedGeom;
            }
            /* else
            {
                osFieldList += "DMGEO2.ST_AsEWKT(";
                osFieldList += osEscapedGeom;
                osFieldList += "::geometry) AS ";
                osFieldList += CPLSPrintf("AsEWKT_%s", poGeomFieldDefn->GetNameRef());
            }*/
        }
        else
        {
            osFieldList += osEscapedGeom;
        }
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        const char *pszName = poFeatureDefn->GetFieldDefn(i)->GetNameRef();

        if (!osFieldList.empty())
            osFieldList += ", ";

        osFieldList += pszName;
    }

    return osFieldList;
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGRDMTableLayer::SetAttributeFilter(const char *pszQuery)

{
    CPLFree(m_pszAttrQueryString);
    m_pszAttrQueryString = (pszQuery) ? CPLStrdup(pszQuery) : nullptr;

    if (pszQuery == nullptr)
        osQuery = "";
    else
        osQuery = pszQuery;

    BuildWhere();

    ResetReading();

    return OGRERR_NONE;
}

/************************************************************************/
/*                           DeleteFeature()                            */
/************************************************************************/

OGRErr OGRDMTableLayer::DeleteFeature(GIntBig nFID)

{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    bAutoFIDOnCreateViaCopy = FALSE;

    /* -------------------------------------------------------------------- */
    /*      We can only delete features if we have a well defined FID       */
    /*      column to target.                                               */
    /* -------------------------------------------------------------------- */
    if (pszFIDColumn == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DeleteFeature(" CPL_FRMT_GIB
                 ") failed.  Unable to delete features in tables without\n"
                 "a recognised FID column.",
                 nFID);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Form the statement to drop the record.                          */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("DELETE FROM \"%s\" WHERE %s = " CPL_FRMT_GIB,
                     pszSqlTableName, pszFIDColumn, nFID);

    /* -------------------------------------------------------------------- */
    /*      Execute the delete.                                             */
    /* -------------------------------------------------------------------- */
    CPLErr rt;
    OGRErr eErr;

    rt = oCommand.Execute(osCommand.c_str());

    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DeleteFeature() DELETE statement failed.");
        eErr = OGRERR_FAILURE;
    }
    else
    {
        eErr = OGRERR_NONE;
    }

    return eErr;
}

/************************************************************************/
/*                             CheckINI()                               */
/************************************************************************/

CPLErr OGRDMTableLayer::CheckINI(int *checkini)

{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLString osCommand;

    osCommand.Printf("SELECT SF_GET_PARA_VALUE(1, 'GEO2_CONS_CHECK')");
    CPLErr eErr = oCommand.Execute(osCommand);
    if (eErr == CE_None)
    {
        char **hResult = oCommand.SimpleFetchRow();
        *checkini = atoi(hResult[0]);
    }
    return eErr;
}
/************************************************************************/
/*                             ISetFeature()                             */
/*                                                                      */
/*      SetFeature() is implemented by an UPDATE SQL command            */
/************************************************************************/

OGRErr OGRDMTableLayer::ISetFeature(OGRFeature *poFeature)
{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLString osCommand;
    int i = 0;
    int bNeedComma = FALSE;
    OGRErr eErr = OGRERR_FAILURE;

    GetLayerDefn()->GetFieldCount();

    if (nullptr == poFeature)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "NULL pointer to OGRFeature passed to SetFeature().");
        return eErr;
    }

    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "FID required on features given to SetFeature().");
        return eErr;
    }

    if (pszFIDColumn == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to update features in tables without\n"
                 "a recognised FID column.");
        return eErr;
    }

    /* In case the FID column has also been created as a regular field */
    if (iFIDAsRegularColumnIndex >= 0)
    {
        if (!poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex) ||
            poFeature->GetFieldAsInteger64(iFIDAsRegularColumnIndex) !=
                poFeature->GetFID())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent values of FID and field of same name");
            return OGRERR_FAILURE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Form the UPDATE command.                                        */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("UPDATE \"%s\" SET ", pszSqlTableName);

    /* Set the geometry */
    for (i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRDMGeomFieldDefn *poGeomFieldDefn =
            poFeatureDefn->GetGeomFieldDefn(i);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_WKB)
        {
            if (bNeedComma)
                osCommand += ", ";
            else
                bNeedComma = TRUE;

            osCommand += poGeomFieldDefn->GetNameRef();
            osCommand += " = ";
            if (poGeom != nullptr)
            {
                char *pszBytea = GeometryToBlob(poGeom);

                if (pszBytea != nullptr)
                {
                    osCommand = osCommand + "'" + pszBytea + "'";
                    CPLFree(pszBytea);
                }
                else
                    osCommand += "NULL";
            }
            else
                osCommand += "NULL";
        }
        else if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY ||
                 poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY)
        {
            if (bNeedComma)
                osCommand += ", ";
            else
                bNeedComma = TRUE;

            osCommand += poGeomFieldDefn->GetNameRef();
            osCommand += " = ";
            if (poGeom != nullptr)
            {
                poGeom->closeRings();
                poGeom->set3D(poGeomFieldDefn->GeometryTypeFlags &
                              OGRGeometry::OGR_G_3D);
                poGeom->setMeasured(poGeomFieldDefn->GeometryTypeFlags &
                                    OGRGeometry::OGR_G_MEASURED);
            }

            if (!CPLTestBool(CPLGetConfigOption("DM_USE_TEXT", "NO")))
            {
                if (poGeom != nullptr)
                {
                    char *pszHexEWKB = OGRGeometryToHexEWKB(
                        poGeom, poGeomFieldDefn->nSRSId, 3, 3);
                    if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY)
                        osCommand += CPLString().Printf(
                            "DMGEO2.ST_GEOGFROMEWKB('%s')", pszHexEWKB);
                    else
                        osCommand += CPLString().Printf(
                            "DMGEO2.ST_GEOMFROMEWKB('%s')", pszHexEWKB);
                    CPLFree(pszHexEWKB);
                }
                else
                    osCommand += "NULL";
            }
            else
            {
                char *pszWKT = nullptr;

                if (poGeom != nullptr)
                    poGeom->exportToWkt(&pszWKT, wkbVariantIso);

                int nSRSId = poGeomFieldDefn->nSRSId;
                if (pszWKT != nullptr)
                {
                    if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY)
                        osCommand += CPLString().Printf(
                            "DMGEO2.ST_GeographyFromText('SRID=%d;%s') ",
                            nSRSId, pszWKT);
                    else
                    {
                        osCommand += CPLString().Printf(
                            "DMGEO2.ST_GeomFromEWKT('SRID=%d;%s') ", nSRSId,
                            pszWKT);
                    }
                    CPLFree(pszWKT);
                }
                else
                    osCommand += "NULL";
            }
        }
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (iFIDAsRegularColumnIndex == i)
            continue;
        if (!poFeature->IsFieldSet(i))
            continue;
        if (m_abGeneratedColumns[i])
            continue;

        if (bNeedComma)
            osCommand += ", ";
        else
            bNeedComma = TRUE;

        osCommand =
            osCommand + poFeatureDefn->GetFieldDefn(i)->GetNameRef() + " = ";

        if (poFeature->IsFieldNull(i))
        {
            osCommand += "NULL";
        }
    }
    if (!bNeedComma)  // nothing to do
        return OGRERR_NONE;

    /* Add the WHERE clause */
    osCommand += " WHERE ";
    osCommand = osCommand + pszFIDColumn + " = ";
    osCommand += CPLString().Printf(CPL_FRMT_GIB, poFeature->GetFID());

    /* -------------------------------------------------------------------- */
    /*      Execute the update.                                             */
    /* -------------------------------------------------------------------- */
    CPLErr rt = oCommand.Execute(osCommand);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "UPDATE command for feature " CPL_FRMT_GIB
                 " failed.\nCommand: %s",
                 poFeature->GetFID(), osCommand.c_str());

        return OGRERR_FAILURE;
    }
    eErr = OGRERR_NONE;

    return eErr;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGRDMTableLayer::ICreateFeature(OGRFeature *poFeature)
{
    GetLayerDefn()->GetFieldCount();

    if (nullptr == poFeature)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "NULL pointer to OGRFeature passed to CreateFeature().");
        return OGRERR_FAILURE;
    }

    /* In case the FID column has also been created as a regular field */
    GIntBig nFID = poFeature->GetFID();
    if (iFIDAsRegularColumnIndex >= 0)
    {
        if (nFID == OGRNullFID)
        {
            if (poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex))
            {
                poFeature->SetFID(poFeature->GetFieldAsInteger64(
                    iFIDAsRegularColumnIndex));
            }
        }
        else
        {
            if (!poFeature->IsFieldSetAndNotNull(iFIDAsRegularColumnIndex) ||
                poFeature->GetFieldAsInteger64(iFIDAsRegularColumnIndex) !=
                    nFID)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Inconsistent values of FID and field of same name");
                return OGRERR_FAILURE;
            }
        }
    }

    if (bFirstInsertion)
    {
        bFirstInsertion = FALSE;
        if (CPLTestBool(CPLGetConfigOption("OGR_TRUNCATE", "NO")))
        {
            OGRDMConn *hDMConn = poDS->GetDMConn();
            OGRDMStatement oCommand(hDMConn);
            CPLString osCommand;
            CPLErr rt;

            osCommand.Printf("TRUNCATE TABLE %s", pszSqlTableName);
            rt = oCommand.Execute(osCommand.c_str());
        }
    }

    OGRErr eErr;
    eErr = CreateFeatureViaInsert(poFeature);

    if (eErr == OGRERR_NONE && iFIDAsRegularColumnIndex >= 0)
    {
        poFeature->SetField(iFIDAsRegularColumnIndex, poFeature->GetFID());
    }

    return eErr;
}

/************************************************************************/
/*                       CreateFeatureViaInsert()                       */
/************************************************************************/

OGRErr OGRDMTableLayer::CreateFeatureViaInsert(OGRFeature *poFeature)

{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    int bNeedComma = FALSE;
    CPLString sql;
    int num = 1;
    /* -------------------------------------------------------------------- */
    /*      Form the INSERT command.                                        */
    /* -------------------------------------------------------------------- */
    if (InsertStatement == nullptr)
    {
        sql.Printf("SELECT NAME FROM SYSCOLUMNS WHERE ID = ("
                   "SELECT o.id FROM SYSOBJECTS o JOIN DBA_TABLES t ON "
                   "o.NAME = t.TABLE_NAME WHERE o.NAME = '%s' AND t.OWNER = "
                   "'%s') ORDER BY COLID; ",
                   pszSqlTableName, pszSchemaName);
        CPLErr eErr = oCommand.Execute(sql);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Can't find columns' name");
            return OGRERR_FAILURE;
        }
        char **hResult = oCommand.SimpleFetchRow();
        hResult = oCommand.SimpleFetchRow();  //skip ogc_fid
        InsertStatement = new OGRDMStatement(hDMConn);
        InsertSQL += " INSERT INTO \"";
        InsertSQL += pszSqlTableName;
        InsertSQL += "\" (";
        sql = ") VALUES(";
        while (hResult)
        {
            if (!bNeedComma)
                bNeedComma = TRUE;
            else
            {
                InsertSQL += ", ";
                sql += ",";
            }
            InsertSQL += hResult[0];
            mymap.insert(std::make_pair(hResult[0], num++));
            sql += "?";
            hResult = oCommand.SimpleFetchRow();
        }
        InsertSQL = InsertSQL + sql + ");";
        InsertStatement->Prepare(InsertSQL);
    }
    CPLErr eErr =
        InsertStatement->Execute_for_insert(poFeatureDefn, poFeature, mymap);

    return eErr;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRDMTableLayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCRandomRead))
    {
        GetLayerDefn()->GetFieldCount();
        return pszFIDColumn != nullptr;
    }

    else if (EQUAL(pszCap, OLCFastFeatureCount) ||
             EQUAL(pszCap, OLCFastSetNextByIndex))
    {
        if (m_poFilterGeom == nullptr)
            return TRUE;
        OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn =
                poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
        return poGeomFieldDefn == nullptr ||
               ((poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY ||
                 poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY));
    }

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
    {
        OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn =
                poFeatureDefn->GetGeomFieldDefn(m_iGeomFieldFilter);
        return poGeomFieldDefn == nullptr ||
               ((poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY ||
                 poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY));
    }

    else if (EQUAL(pszCap, OLCTransactions))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastGetExtent))
    {
        OGRDMGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (poFeatureDefn->GetGeomFieldCount() > 0)
            poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(0);
        return poGeomFieldDefn != nullptr &&
               poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY;
    }

    else if (EQUAL(pszCap, OLCStringsAsUTF8))
        return TRUE;

    else if (EQUAL(pszCap, OLCCurveGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCMeasuredGeometries))
        return TRUE;

    else
        return FALSE;
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGRDMTableLayer::CreateField(const OGRFieldDefn *poFieldIn,
                                    int bApproxOK)

{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    CPLString osCommand;
    CPLString osFieldType;
    OGRFieldDefn oField(poFieldIn);

    GetLayerDefn()->GetFieldCount();

    if (pszFIDColumn != nullptr && EQUAL(oField.GetNameRef(), pszFIDColumn) &&
        oField.GetType() != OFTInteger && oField.GetType() != OFTInteger64)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for %s",
                 oField.GetNameRef());
        return OGRERR_FAILURE;
    }

    const char *pszOverrideType =
        CSLFetchNameValue(papszOverrideColumnTypes, oField.GetNameRef());
    if (pszOverrideType != nullptr)
        osFieldType = pszOverrideType;
    else
    {
        osFieldType = OGRDMCommonLayerGetType(
            oField, CPL_TO_BOOL(bPreservePrecision), CPL_TO_BOOL(bApproxOK));
        if (osFieldType.empty())
            return OGRERR_FAILURE;
    }

    CPLString osConstraints;
    if (!oField.IsNullable())
        osConstraints += " NOT NULL";
    if (oField.IsUnique())
        osConstraints += " UNIQUE";
    if (oField.GetDefault() != nullptr && !oField.IsDefaultDriverSpecific())
    {
        osConstraints += " DEFAULT ";
        osConstraints += oField.GetDefault();
    }

    /* -------------------------------------------------------------------- */
    /*      Create the new field.                                           */
    /* -------------------------------------------------------------------- */
    osCommand.Printf("ALTER TABLE \"%s\" ADD COLUMN %s %s", pszSqlTableName,
                     oField.GetNameRef(), osFieldType.c_str());
    osCommand += osConstraints;

    OGRDMStatement oCommand(hDMConn);
    CPLErr eErr = oCommand.Execute(osCommand);

    if (eErr != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                 osCommand.c_str());
        return OGRERR_FAILURE;
    }

    poFeatureDefn->AddFieldDefn(&oField);
    m_abGeneratedColumns.resize(poFeatureDefn->GetFieldCount());

    if (pszFIDColumn != nullptr && EQUAL(oField.GetNameRef(), pszFIDColumn))
    {
        iFIDAsRegularColumnIndex = poFeatureDefn->GetFieldCount() - 1;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                        RunAddGeometryColumn()                        */
/************************************************************************/

OGRErr OGRDMTableLayer::RunAddGeometryColumn(const OGRDMGeomFieldDefn *poGeomField)
{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);

    const char *pszGeometryType = OGRToOGCGeomType(poGeomField->GetType());
    const char *suffix = "";
    int dim = 2;
    if ((poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D) &&
        (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
        dim = 4;
    else if ((poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED))
    {
        if (!(wkbFlatten(poGeomField->GetType()) == wkbUnknown))
            suffix = "M";
        dim = 3;
    }
    else if (poGeomField->GeometryTypeFlags & OGRGeometry::OGR_G_3D)
        dim = 3;

    CPLString osCommand;
    osCommand.Printf("ALTER TABLE \"%s\".\"%s\" ADD COLUMN %s "
                     "SYSGEO2.ST_GEOMETRY CHECK(SRID=%d) CHECK(TYPE=%s%s)",
                     pszSchemaName, pszTableName, poGeomField->GetNameRef(),
                     poGeomField->nSRSId, pszGeometryType, suffix);

    CPLErr eErr = oCommand.Execute(osCommand);
    if (eErr != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "AddGeometryColumn failed for layer %s.", GetName());

        return OGRERR_FAILURE;
    }
    if (!poGeomField->IsNullable())
    {
        osCommand.Printf("ALTER TABLE \"%s\" ALTER COLUMN %s SET NOT NULL",
                         pszSqlTableName, poGeomField->GetNameRef());

        eErr = oCommand.Execute(osCommand);
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                           CreateGeomField()                          */
/************************************************************************/

OGRErr OGRDMTableLayer::CreateGeomField(const OGRGeomFieldDefn *poGeomFieldIn,
                                        CPL_UNUSED int bApproxOK)
{
    OGRwkbGeometryType eType = poGeomFieldIn->GetType();
    if (eType == wkbNone)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot create geometry field of type wkbNone");
        return OGRERR_FAILURE;
    }

    // Check if GEOMETRY_NAME layer creation option was set, but no initial
    // column was created in ICreateLayer()
    CPLString osGeomFieldName = (m_osFirstGeometryFieldName.size())
                                    ? m_osFirstGeometryFieldName
                                    : CPLString(poGeomFieldIn->GetNameRef());
    m_osFirstGeometryFieldName = "";  // reset for potential next geom columns

    auto poGeomField =
        std::make_unique<OGRDMGeomFieldDefn>(this, osGeomFieldName);
    if (EQUAL(poGeomField->GetNameRef(), ""))
    {
        if (poFeatureDefn->GetGeomFieldCount() == 0)
            poGeomField->SetName("wkb_geometry");
        else
            poGeomField->SetName(CPLSPrintf(
                "wkb_geometry%d", poFeatureDefn->GetGeomFieldCount() + 1));
    }
    const auto poSRSIn = poGeomFieldIn->GetSpatialRef();
    if (poSRSIn)
    {
        auto l_poSRS = poSRSIn->Clone();
        l_poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        poGeomField->SetSpatialRef(l_poSRS);
        l_poSRS->Release();
    }

    const OGRSpatialReference *poSRS = poGeomField->GetSpatialRef();
    int nSRSId = poDS->GetUndefinedSRID();
    if (nForcedSRSId != UNDETERMINED_SRID)
        nSRSId = nForcedSRSId;
    else if (poSRS != nullptr)
        nSRSId = poDS->FetchSRSId(poSRS);

    int GeometryTypeFlags = 0;
    if (OGR_GT_HasZ(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_3D;
    if (OGR_GT_HasM(eType))
        GeometryTypeFlags |= OGRGeometry::OGR_G_MEASURED;
    if (nForcedGeometryTypeFlags >= 0)
    {
        GeometryTypeFlags = nForcedGeometryTypeFlags;
        eType =
            OGR_GT_SetModifier(eType, GeometryTypeFlags & OGRGeometry::OGR_G_3D,
                               GeometryTypeFlags & OGRGeometry::OGR_G_MEASURED);
    }
    poGeomField->SetType(eType);
    poGeomField->SetNullable(poGeomFieldIn->IsNullable());
    poGeomField->nSRSId = nSRSId;
    poGeomField->GeometryTypeFlags = GeometryTypeFlags;
    poGeomField->eDMGeoType = GEOM_TYPE_GEOMETRY;

    /* -------------------------------------------------------------------- */
    /*      Create the new field.                                           */
    /* -------------------------------------------------------------------- */
    if (!bDeferredCreation)
    {
        if (RunAddGeometryColumn(poGeomField.get()) != OGRERR_NONE)
        {
            return OGRERR_FAILURE;
        }
    }

    poFeatureDefn->AddGeomFieldDefn(std::move(poGeomField));

    return OGRERR_NONE;
}

/************************************************************************/
/*                            DeleteField()                             */
/************************************************************************/

OGRErr OGRDMTableLayer::DeleteField(int iField)
{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLErr eErr;
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    if (iField < 0 || iField >= poFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    osCommand.Printf("ALTER TABLE \"%s\" DROP COLUMN %s", pszSqlTableName,
                     poFeatureDefn->GetFieldDefn(iField)->GetNameRef());
    eErr = oCommand.Execute(osCommand);
    if (eErr != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!", osCommand.c_str());
        return OGRERR_FAILURE;
    }

    m_abGeneratedColumns.erase(m_abGeneratedColumns.begin() + iField);

    return poFeatureDefn->DeleteFieldDefn(iField);
}

/************************************************************************/
/*                           AlterFieldDefn()                           */
/************************************************************************/

OGRErr OGRDMTableLayer::AlterFieldDefn(int iField,
                                       OGRFieldDefn *poNewFieldDefn,
                                       int nFlagsIn)
{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLErr eErr;
    CPLString osCommand;

    GetLayerDefn()->GetFieldCount();

    if (iField < 0 || iField >= poFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    OGRFieldDefn *poFieldDefn = poFeatureDefn->GetFieldDefn(iField);
    OGRFieldDefn oField(poNewFieldDefn);

    if (!(nFlagsIn & ALTER_TYPE_FLAG))
    {
        oField.SetSubType(OFSTNone);
        oField.SetType(poFieldDefn->GetType());
        oField.SetSubType(poFieldDefn->GetSubType());
    }

    if (!(nFlagsIn & ALTER_WIDTH_PRECISION_FLAG))
    {
        oField.SetWidth(poFieldDefn->GetWidth());
        oField.SetPrecision(poFieldDefn->GetPrecision());
    }

    if ((nFlagsIn & ALTER_TYPE_FLAG) || (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG))
    {
        CPLString osFieldType = OGRDMCommonLayerGetType(
            oField, CPL_TO_BOOL(bPreservePrecision), true);
        if (osFieldType.empty())
        {
            return OGRERR_FAILURE;
        }

        osCommand.Printf("ALTER TABLE \"%s\" ALTER COLUMN %s TYPE %s",
                         pszSqlTableName, poFieldDefn->GetNameRef(),
                         osFieldType.c_str());

        eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                     osCommand.c_str());
            return OGRERR_FAILURE;
        }
    }

    if ((nFlagsIn & ALTER_NULLABLE_FLAG) &&
        poFieldDefn->IsNullable() != poNewFieldDefn->IsNullable())
    {
        oField.SetNullable(poNewFieldDefn->IsNullable());

        if (poNewFieldDefn->IsNullable())
            osCommand.Printf("ALTER TABLE \"%s\" ALTER COLUMN %s DROP NOT NULL",
                             pszSqlTableName, poFieldDefn->GetNameRef());
        else
            osCommand.Printf("ALTER TABLE \"%s\" ALTER COLUMN %s SET NOT NULL",
                             pszSqlTableName, poFieldDefn->GetNameRef());

        eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                     osCommand.c_str());
            return OGRERR_FAILURE;
        }
    }

    // Only supports adding a unique constraint
    if ((nFlagsIn & ALTER_UNIQUE_FLAG) && !poFieldDefn->IsUnique() &&
        poNewFieldDefn->IsUnique())
    {
        oField.SetUnique(poNewFieldDefn->IsUnique());

        osCommand.Printf("ALTER TABLE \"%s\" ADD UNIQUE (%s)", pszSqlTableName,
                         poFieldDefn->GetNameRef());

        eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                     osCommand.c_str());
            return OGRERR_FAILURE;
        }
    }
    else if ((nFlagsIn & ALTER_UNIQUE_FLAG) && poFieldDefn->IsUnique() &&
             !poNewFieldDefn->IsUnique())
    {
        oField.SetUnique(TRUE);
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Dropping a UNIQUE constraint is not supported currently");
    }

    if ((nFlagsIn & ALTER_DEFAULT_FLAG) &&
        ((poFieldDefn->GetDefault() == nullptr &&
          poNewFieldDefn->GetDefault() != nullptr) ||
         (poFieldDefn->GetDefault() != nullptr &&
          poNewFieldDefn->GetDefault() == nullptr) ||
         (poFieldDefn->GetDefault() != nullptr &&
          poNewFieldDefn->GetDefault() != nullptr &&
          strcmp(poFieldDefn->GetDefault(), poNewFieldDefn->GetDefault()) !=
              0)))
    {
        oField.SetDefault(poNewFieldDefn->GetDefault());

        if (poNewFieldDefn->GetDefault() == nullptr)
            osCommand.Printf("ALTER TABLE \"%s\" ALTER COLUMN %s DROP DEFAULT",
                             pszSqlTableName, poFieldDefn->GetNameRef());
        else
            osCommand.Printf(
                "ALTER TABLE \"%s\" ALTER COLUMN %s SET DEFAULT %s",
                pszSqlTableName, poFieldDefn->GetNameRef(),
                poFieldDefn->GetDefault());

        eErr = oCommand.Execute(osCommand);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                     osCommand.c_str());
            return OGRERR_FAILURE;
        }
    }

    if ((nFlagsIn & ALTER_NAME_FLAG))
    {
        if (EQUAL(oField.GetNameRef(), "oid"))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Renaming field 'oid' to 'oid_' to avoid conflict with "
                     "internal oid field.");
            oField.SetName("oid_");
        }

        if (strcmp(poFieldDefn->GetNameRef(), oField.GetNameRef()) != 0)
        {
            osCommand.Printf("ALTER TABLE \"%s\" RENAME COLUMN %s TO %s",
                             pszSqlTableName, poFieldDefn->GetNameRef(),
                             oField.GetNameRef());
            eErr = oCommand.Execute(osCommand);
            if (eErr != CE_None)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "%s Failed!",
                         osCommand.c_str());
                return OGRERR_FAILURE;
            }
        }
    }

    if (nFlagsIn & ALTER_NAME_FLAG)
        poFieldDefn->SetName(oField.GetNameRef());
    if (nFlagsIn & ALTER_TYPE_FLAG)
    {
        poFieldDefn->SetSubType(OFSTNone);
        poFieldDefn->SetType(oField.GetType());
        poFieldDefn->SetSubType(oField.GetSubType());
    }
    if (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG)
    {
        poFieldDefn->SetWidth(oField.GetWidth());
        poFieldDefn->SetPrecision(oField.GetPrecision());
    }
    if (nFlagsIn & ALTER_NULLABLE_FLAG)
        poFieldDefn->SetNullable(oField.IsNullable());
    if (nFlagsIn & ALTER_DEFAULT_FLAG)
        poFieldDefn->SetDefault(oField.GetDefault());
    if (nFlagsIn & ALTER_UNIQUE_FLAG)
        poFieldDefn->SetUnique(oField.IsUnique());

    return OGRERR_NONE;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRDMTableLayer::GetFeature(GIntBig nFeatureId)

{
    GetLayerDefn()->GetFieldCount();

    if (pszFIDColumn == nullptr)
        return OGRLayer::GetFeature(nFeatureId);

    /* -------------------------------------------------------------------- */
    /*      Issue query for a single record.                                */
    /* -------------------------------------------------------------------- */
    OGRFeature *poFeature = nullptr;
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLErr eErr;
    CPLString osFieldList = BuildFields();
    CPLString osCommand;
    DPIRETURN rt;

    osCommand.Printf("DECLARE getfeaturecursor %s for "
                     "SELECT %s FROM \"%s\" WHERE %s = " CPL_FRMT_GIB,
                     (poDS->bUseBinaryCursor) ? "BINARY CURSOR" : "CURSOR",
                     osFieldList.c_str(), pszSqlTableName, pszFIDColumn,
                     nFeatureId);

    eErr = oCommand.Execute(osCommand);

    if (eErr == CE_None)
    {
        eErr = oCommand.Execute("FETCH ALL in getfeaturecursor");

        if (eErr == CE_None)
        {
            sdint2 nParams;
            rt = dpi_number_params(oCommand.GetStatement(), &nParams);
            if (nParams > 0)
            {
                int *panTempMapFieldNameToIndex = nullptr;
                int *panTempMapFieldNameToGeomIndex = nullptr;
                CreateMapFromFieldNameToIndex(&oCommand, poFeatureDefn,
                                              panTempMapFieldNameToIndex,
                                              panTempMapFieldNameToGeomIndex);
                poFeature =
                    RecordToFeature(&oCommand, panTempMapFieldNameToIndex,
                                    panTempMapFieldNameToGeomIndex, 0);
                CPLFree(panTempMapFieldNameToIndex);
                CPLFree(panTempMapFieldNameToGeomIndex);
                if (poFeature && iFIDAsRegularColumnIndex >= 0)
                {
                    poFeature->SetField(iFIDAsRegularColumnIndex,
                                        poFeature->GetFID());
                }

                if (nParams > 1)
                {
                    CPLError(
                        CE_Warning, CPLE_AppDefined,
                        "%d rows in response to the WHERE %s = " CPL_FRMT_GIB
                        " clause !",
                        nParams, pszFIDColumn, nFeatureId);
                }
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Attempt to read feature with unknown feature id "
                         "(" CPL_FRMT_GIB ").",
                         nFeatureId);
            }
        }
    }
    else if (eErr != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Get Feature Failed!");
    }

    return poFeature;
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRDMTableLayer::GetFeatureCount(int bForce)

{
    /* -------------------------------------------------------------------- */
    /*      In theory it might be wise to cache this result, but it         */
    /*      won't be trivial to work out the lifetime of the value.         */
    /*      After all someone else could be adding records from another     */
    /*      application when working against a database.                    */
    /* -------------------------------------------------------------------- */
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLString osCommand;
    GIntBig nCount = 0;

    osCommand.Printf("SELECT count(*) FROM \"%s\" %s", pszSqlTableName,
                     osWHERE.c_str());

    CPLErr rt = oCommand.Execute(osCommand);
    char **hResult = oCommand.SimpleFetchRow();
    if (hResult != nullptr && !DSQL_SUCCEEDED(rt))
        nCount = CPLAtoGIntBig(hResult[0]);
    else
        CPLDebug("DM", "%s; failed.", osCommand.c_str());

    return nCount;
}

/************************************************************************/
/*                             ResolveSRID()                            */
/************************************************************************/

void OGRDMTableLayer::ResolveSRID(const OGRDMGeomFieldDefn *poGFldDefn)

{
    OGRDMConn *hDMConn = poDS->GetDMConn();
    OGRDMStatement oCommand(hDMConn);
    CPLString osCommand;
    DPIRETURN rt;
    sdint2 nParams;

    int nSRSId = poDS->GetUndefinedSRID();
    if (!poDS->m_bHasGeometryColumns)
    {
        poGFldDefn->nSRSId = nSRSId;
        return;
    }

    osCommand.Printf("SELECT srid FROM SYSGEO2.GEOMETRY_COLUMNS "
                     "WHERE f_table_name = '%s' AND "
                     "f_geometry_column = '%s'",
                     pszTableName, poGFldDefn->GetNameRef());

    osCommand +=
        CPLString().Printf(" AND f_table_schema = '%s'", pszSchemaName);

    CPLErr eErr = oCommand.Execute(osCommand);
    rt = dpi_number_params(oCommand.GetStatement(), &nParams);
    char **hResult = oCommand.SimpleFetchRow();

    if (eErr == CE_None && DSQL_SUCCEEDED(rt) && nParams == 1)
    {
        nSRSId = atoi(hResult[0]);
    }

    /* SRID = 0 can also mean that there's no constraint */
    /* so we need to fetch from values */
    /* We assume that all geometry of this column have identical SRID */
    if (nSRSId <= 0 && poGFldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY)
    {
        const char *psGetSRIDFct = "DMGEO2.ST_SRID";

        CPLString osGetSRID;
        osGetSRID += "SELECT ";
        osGetSRID += psGetSRIDFct;
        osGetSRID += "(";
        osGetSRID += poGFldDefn->GetNameRef();
        osGetSRID += ") FROM \"";
        osGetSRID += pszSqlTableName;
        osGetSRID += "\" WHERE (";
        osGetSRID += poGFldDefn->GetNameRef();
        osGetSRID += " IS NOT NULL) LIMIT 1";

        eErr = oCommand.Execute(osCommand);
        rt = dpi_number_params(oCommand.GetStatement(), &nParams);
        hResult = oCommand.SimpleFetchRow();
        if (hResult && eErr == CE_None && DSQL_SUCCEEDED(rt) && nParams == 1)
        {
            nSRSId = atoi(hResult[0]);
        }
    }

    poGFldDefn->nSRSId = nSRSId;
}

/************************************************************************/
/*                    CheckGeomTypeCompatibility()                      */
/************************************************************************/

void OGRDMTableLayer::CheckGeomTypeCompatibility(int iGeomField,
                                                 OGRGeometry *poGeom)
{
    if (bHasWarnedIncompatibleGeom)
        return;

    OGRwkbGeometryType eExpectedGeomType =
        poFeatureDefn->GetGeomFieldDefn(iGeomField)->GetType();
    OGRwkbGeometryType eFlatLayerGeomType = wkbFlatten(eExpectedGeomType);
    OGRwkbGeometryType eFlatGeomType = wkbFlatten(poGeom->getGeometryType());
    if (eFlatLayerGeomType == wkbUnknown)
        return;

    if (eFlatLayerGeomType == wkbGeometryCollection)
        bHasWarnedIncompatibleGeom = eFlatGeomType != wkbMultiPoint &&
                                     eFlatGeomType != wkbMultiLineString &&
                                     eFlatGeomType != wkbMultiPolygon &&
                                     eFlatGeomType != wkbGeometryCollection;
    else
        bHasWarnedIncompatibleGeom = (eFlatGeomType != eFlatLayerGeomType);

    if (bHasWarnedIncompatibleGeom)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Geometry to be inserted is of type %s, whereas the layer "
                 "geometry type is %s.\n"
                 "Insertion is likely to fail",
                 OGRGeometryTypeToName(poGeom->getGeometryType()),
                 OGRGeometryTypeToName(eExpectedGeomType));
    }
}

/************************************************************************/
/*                        SetOverrideColumnTypes()                      */
/************************************************************************/

void OGRDMTableLayer::SetOverrideColumnTypes(const char *pszOverrideColumnTypes)
{
    if (pszOverrideColumnTypes == nullptr)
        return;

    const char *pszIter = pszOverrideColumnTypes;
    CPLString osCur;
    while (*pszIter != '\0')
    {
        if (*pszIter == '(')
        {
            /* Ignore commas inside ( ) pair */
            while (*pszIter != '\0')
            {
                if (*pszIter == ')')
                {
                    osCur += *pszIter;
                    pszIter++;
                    break;
                }
                osCur += *pszIter;
                pszIter++;
            }
            if (*pszIter == '\0')
                break;
        }

        if (*pszIter == ',')
        {
            papszOverrideColumnTypes =
                CSLAddString(papszOverrideColumnTypes, osCur);
            osCur = "";
        }
        else
            osCur += *pszIter;
        pszIter++;
    }
    if (!osCur.empty())
        papszOverrideColumnTypes =
            CSLAddString(papszOverrideColumnTypes, osCur);
}

/************************************************************************/
/*                             GetExtent()                              */
/*                                                                      */
/*      For DMGEO2 use internal ST_EstimatedExtent(geometry) function   */
/*      if bForce == 0                                                  */
/************************************************************************/

OGRErr OGRDMTableLayer::GetExtent(int iGeomField,
                                  OGREnvelope *psExtent,
                                  int bForce)
{
    CPLString osCommand;

    if (iGeomField < 0 || iGeomField >= GetLayerDefn()->GetGeomFieldCount() ||
        GetLayerDefn()->GetGeomFieldDefn(iGeomField)->GetType() == wkbNone)
    {
        if (iGeomField != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid geometry field index : %d", iGeomField);
        }
        return OGRERR_FAILURE;
    }

    OGRDMGeomFieldDefn *poGeomFieldDefn =
        poFeatureDefn->GetGeomFieldDefn(iGeomField);

    if (bForce == 0 && poGeomFieldDefn->eDMGeoType != GEOM_TYPE_GEOGRAPHY)
    {
        osCommand.Printf(
            "SELECT DMGEO2.ST_EstimatedExtent(%s, %s, %s).ST_ASTEXT",
            pszSchemaName, pszTableName, poGeomFieldDefn->GetNameRef());
        if (RunGetExtentRequest(psExtent, bForce, osCommand, TRUE) ==
            OGRERR_NONE)
            return OGRERR_NONE;

        CPLDebug(
            "DM",
            "Unable to get estimated extent by DMGEO2. Trying real extent.");
    }

    return OGRDMLayer::GetExtent(psExtent, bForce);
}

/************************************************************************/
/*                           Rename()                                   */
/************************************************************************/

OGRErr OGRDMTableLayer::Rename(const char *pszNewName)
{
    ResetReading();

    char *pszNewSqlTableName = CPLStrdup(pszNewName);
    OGRDMConn *hDMConn = poDS->GetDMConn();
    CPLString osCommand;
    osCommand.Printf("ALTER TABLE \"%s\" RENAME TO \"%s\"", pszSqlTableName,
                     pszNewSqlTableName);
    OGRDMStatement oCommand(hDMConn);
    CPLErr rt = oCommand.Execute(osCommand);

    OGRErr eRet = OGRERR_NONE;
    if (!DSQL_SUCCEEDED(rt))
    {
        eRet = OGRERR_FAILURE;
        CPLError(CE_Failure, CPLE_AppDefined, "Rename Failed!");

        CPLFree(pszNewSqlTableName);
    }
    else
    {
        CPLFree(pszTableName);
        pszTableName = CPLStrdup(pszNewName);

        CPLFree(pszSqlTableName);
        pszSqlTableName = pszNewSqlTableName;

        SetDescription(pszNewName);
        poFeatureDefn->SetName(pszNewName);
    }

    return eRet;
}

/************************************************************************/
/*                        SetDeferredCreation()                         */
/************************************************************************/

void OGRDMTableLayer::SetDeferredCreation(CPLString osCreateTableIn)
{
    osCreateTable = osCreateTableIn;
}
