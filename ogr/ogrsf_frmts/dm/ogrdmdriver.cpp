#include "ogr_dm.h"

CPL_CVSID("$Id$")

/************************************************************************/
/*                         OGRDMDriverIdentify()                        */
/************************************************************************/

static int OGRDMDriverIdentify(GDALOpenInfo *poOpenInfo)
{
    return STARTS_WITH_CI(poOpenInfo->pszFilename, "DM:");
}

/************************************************************************/
/*                           OGRDMDriverOpen()                          */
/************************************************************************/

static GDALDataset *OGRDMDriverOpen(GDALOpenInfo *poOpenInfo)

{
    if (!OGRDMDriverIdentify(poOpenInfo))
        return nullptr;

    OGRDMDataSource *poDS;

    poDS = new OGRDMDataSource();

    if (!poDS->Open(poOpenInfo->pszFilename, poOpenInfo->eAccess == GA_Update,
                    TRUE, poOpenInfo->papszOpenOptions))
    {
        delete poDS;
        return nullptr;
    }
    else
        return poDS;
}

/************************************************************************/
/*                         OGRDMDriverCreate()                          */
/************************************************************************/

static GDALDataset *OGRDMDriverCreate(const char *pszName,
                                      CPL_UNUSED int nBands,
                                      CPL_UNUSED int nXSize,
                                      CPL_UNUSED int nYSize,
                                      CPL_UNUSED GDALDataType eDT,
                                      char **papszOptions)

{
    OGRDMDataSource *poDS;

    poDS = new OGRDMDataSource();

    if (!poDS->Open(pszName, TRUE, TRUE, papszOptions))
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DaMeng driver doesn't currently support database creation.\n"
                 "Please create database with the DM tools before loading "
                 "tables.");
        return nullptr;
    }

    return poDS;
}

/************************************************************************/
/*                           RegisterOGRDM()                            */
/************************************************************************/

void RegisterOGRDM()

{
    if (!GDAL_CHECK_VERSION("DM driver"))
        return;

    if (GDALGetDriverByName("DM") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("DM");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "DMGEO2");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "");
    poDriver->SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, "DM:");

    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='DBNAME' type='string' description='Database name'/>"
        "  <Option name='USER' type='string' description='User name'/>"
        "  <Option name='PASSWORD' type='string' description='Password'/>"
        "  <Option name='TABLES' type='string' description='Restricted set of "
        "tables to list (comma separated)'/>"
        "  <Option name='INSERTNUM' type='boolean' description='Whether all "
        "tables, including non-spatial ones, should be listed' default='NO'/>"
        "</OpenOptionList>");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST,
                              "<CreationOptionList/>");

    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='GEOM_TYPE' type='string-select' description='Format "
        "of geometry columns' default='geometry'>"
        "    <Value>geometry</Value>"
        "    <Value>geography</Value>"
        "  </Option>"
        "  <Option name='OVERWRITE' type='boolean' description='Whether to "
        "overwrite an existing table with the layer name to be created' "
        "default='NO'/>"
        "  <Option name='LAUNDER' type='boolean' description='Whether layer "
        "and field names will be laundered' default='YES'/>"
        "  <Option name='PRECISION' type='boolean' description='Whether fields "
        "created should keep the width and precision' default='YES'/>"
        "  <Option name='DIM' type='string' description='Set to 2 to force the "
        "geometries to be 2D, 3 to be 2.5D, XYM or XYZM'/>"
        "  <Option name='GEOMETRY_NAME' type='string' description='Name of "
        "geometry column. Defaults to wkb_geometry for GEOM_TYPE=geometry or "
        "the_geog for GEOM_TYPE=geography'/>"
        "  <Option name='SPATIAL_INDEX' type='boolean' description='Type of "
        "spatial index to create' default='GIST'/>"
        "  <Option name='FID' type='string' description='Name of the FID "
        "column to create' default='ogc_fid'/>"
        "  <Option name='FID64' type='boolean' description='Whether to create "
        "the FID column with BIGSERIAL type to handle 64bit wide ids' "
        "default='NO'/>"
        "  <Option name='DESCRIPTION' type='string' description='Description "
        "string to put in the all_tab_comments system table'/>"
        "</LayerCreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 Real String Date DateTime "
                              "Time Binary");
    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DEFAULT_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_GEOMFIELDS, "YES");

    poDriver->pfnOpen = OGRDMDriverOpen;
    poDriver->pfnIdentify = OGRDMDriverIdentify;
    poDriver->pfnCreate = OGRDMDriverCreate;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
