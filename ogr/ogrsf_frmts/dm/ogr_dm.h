#ifndef OGR_DM_H_INCLUDED
#define OGR_DM_H_INCLUDED

#include "ogrsf_frmts.h"
#include "cpl_string.h"
#include "DPI.h"
#include "DPIext.h"
#include "DPItypes.h"

#define UNDETERMINED_SRID -2
#define NDCT_IDCLS_PACKAGE 14
#define NDCT_PKGID_DMGEO2 (NDCT_IDCLS_PACKAGE << 24 | 112)
#define NDCT_CLSID_GEO2_ST_GEOMETRY (NDCT_IDCLS_PACKAGE << 24 | 113)
#define NDCT_CLSID_GEO2_ST_POINT (NDCT_IDCLS_PACKAGE << 24 | 114)
#define NDCT_CLSID_GEO2_ST_LINE (NDCT_IDCLS_PACKAGE << 24 | 115)
#define NDCT_CLSID_GEO2_ST_POLYGON (NDCT_IDCLS_PACKAGE << 24 | 116)
#define NDCT_CLSID_GEO2_ST_MULTIPOINT (NDCT_IDCLS_PACKAGE << 24 | 117)
#define NDCT_CLSID_GEO2_ST_MULTILINE (NDCT_IDCLS_PACKAGE << 24 | 118)
#define NDCT_CLSID_GEO2_ST_MULTIPOLYGON (NDCT_IDCLS_PACKAGE << 24 | 119)
#define NDCT_CLSID_GEO2_ST_COLLECTION (NDCT_IDCLS_PACKAGE << 24 | 120)
#define NDCT_CLSID_GEO2_ST_CIRCSTRING (NDCT_IDCLS_PACKAGE << 24 | 121)
#define NDCT_CLSID_GEO2_ST_COMPOUND (NDCT_IDCLS_PACKAGE << 24 | 122)
#define NDCT_CLSID_GEO2_ST_CURVEPOLY (NDCT_IDCLS_PACKAGE << 24 | 123)
#define NDCT_CLSID_GEO2_ST_MULTICURVE (NDCT_IDCLS_PACKAGE << 24 | 124)
#define NDCT_CLSID_GEO2_ST_MULTISURFACE (NDCT_IDCLS_PACKAGE << 24 | 125)
#define NDCT_CLSID_GEO2_ST_POLYHEDRALSURFACE (NDCT_IDCLS_PACKAGE << 24 | 126)
#define NDCT_CLSID_GEO2_ST_TRIANGLE (NDCT_IDCLS_PACKAGE << 24 | 127)
#define NDCT_CLSID_GEO2_ST_TIN (NDCT_IDCLS_PACKAGE << 24 | 128)
#define NDCT_CLSID_GEO2_ST_GEOGRAPHY (NDCT_IDCLS_PACKAGE << 24 | 129)

#define fetchnum 100000
#define FORCED_COMMIT_NUM 10
#define FORCED_INSERT_NUM 300

extern int ogr_DM_insertnum;
class OGRDMDataSource;
class OGRDMLayer;

typedef enum
{
    GEOM_TYPE_UNKNOWN = 0,
    GEOM_TYPE_GEOMETRY = 1,
    GEOM_TYPE_GEOGRAPHY = 2,
    GEOM_TYPE_WKB = 3
} DMGeoType;

typedef struct
{
    unsigned int size; /* For DMGEO2 use only, use VAR* macros to manipulate. */
    unsigned char srid[3]; /* 24 bits of SRID */
    unsigned char gflags;  /* HasZ, HasM, HasBBox, IsGeodetic */
    unsigned char data[1]; /* See gserialized.txt */
} GSERIALIZED;

typedef struct
{
    char *pszName;
    char *pszGeomType;
    int GeometryTypeFlags;
    int nSRID;
    DMGeoType eDMGeoType;
    int bNullable;
} DMGeomColumnDesc;

class CPL_DLL OGRDMConn
{
  public:
    dhenv hEnv;
    DPIRETURN hRtn;
    dhcon hCon;

    char *pszUserid;
    char *pszPassword;
    char *pszDatabase;

  public:
    OGRDMConn();
    virtual ~OGRDMConn();
    int EstablishConn(const char *pszUserid, const char *pszPassword,
                      const char *pszDatabase);
};

typedef struct
{
    sdbyte name[128 + 1];
    sdint2 nameLen;

    sdint2 sql_type;
    ulength prec;
    sdint2 scale;
    sdint2 nullable;

    slength display_size;
} DmColDesc;

class OGRDMGeomFieldDefn final : public OGRGeomFieldDefn
{
    OGRDMGeomFieldDefn(const OGRDMGeomFieldDefn &) = delete;
    OGRDMGeomFieldDefn &operator=(const OGRDMGeomFieldDefn &) = delete;

  protected:
    OGRDMLayer *poLayer;

  public:
    OGRDMGeomFieldDefn(OGRDMLayer *poLayerIn,
                       const char *pszFieldName)
        : OGRGeomFieldDefn(pszFieldName, wkbUnknown), poLayer(poLayerIn),
          nSRSId(UNDETERMINED_SRID), GeometryTypeFlags(0),
          eDMGeoType(GEOM_TYPE_UNKNOWN)
    {
    }

    virtual const OGRSpatialReference *GetSpatialRef() const override;

    void UnsetLayer()
    {
        poLayer = nullptr;
    }

    mutable int nSRSId;
    mutable int GeometryTypeFlags;
    mutable DMGeoType eDMGeoType;
};

class OGRDMFeatureDefn CPL_NON_FINAL : public OGRFeatureDefn
{
  public:
    explicit OGRDMFeatureDefn(const char *pszName = nullptr)
        : OGRFeatureDefn(pszName)
    {
        SetGeomType(wkbNone);
    }

    virtual void UnsetLayer()
    {
        const int nGeomFieldCount = GetGeomFieldCount();
        for (int i = 0; i < nGeomFieldCount; i++)       
            cpl::down_cast<OGRDMGeomFieldDefn *>(apoGeomFieldDefn[i].get())
                ->UnsetLayer();
    }

    OGRDMGeomFieldDefn *GetGeomFieldDefn(int i) override
    {
        return cpl::down_cast<OGRDMGeomFieldDefn *>(
            OGRFeatureDefn::GetGeomFieldDefn(i));
    }

    const OGRDMGeomFieldDefn *GetGeomFieldDefn(int i) const override
    {
        return cpl::down_cast<const OGRDMGeomFieldDefn *>(
            OGRFeatureDefn::GetGeomFieldDefn(i));
    }
};

class CPL_DLL OGRDMStatement
{
  public:
    explicit OGRDMStatement(OGRDMConn *);
    virtual ~OGRDMStatement();
    dhstmt *GetStatement()
    {
        return &hStatement;
    }
    char *pszCommandText;
    CPLErr Prepare(const char *pszStatement);
    CPLErr ExecuteInsert(const char *pszSQLStatement, int nMode);
    CPLErr Execute(const char *pszStatement, int nMode = -1);
    CPLErr Excute_for_fetchmany(const char *pszStatement);
    void Clean();
    char **SimpleFetchRow();
    char ***Fetchmany(ulength *rows);
    int GetColCount() const
    {
        return nRawColumnCount;
    }
    int *blob_len;
    int **blob_lens;
    CPLErr Execute_for_insert(OGRDMFeatureDefn *params,
                              OGRFeature *poFeature,
                              std::map<std::string, int> mymap);

  private:
    OGRDMConn *poConn;
    dhstmt hStatement;
    int nRawColumnCount;
    char **result;
    char **papszCurImage;
    int *object_index;
    int *lob_index;
    dhobjdesc *objdesc;
    dhobj *obj;
    dhloblctr *lob;
    //used for fetchmany
    int is_fectmany;
    char ***results;
    dhobj **objs;
    dhloblctr **lobs;
    dhobjdesc **objdescs;
    char ***papszCurImages;
    int param_nums = 0;
    DmColDesc *paramdescs = nullptr;
    dhobj **insert_objs;
    dhobjdesc insert_objdesc;
    GSERIALIZED ***insert_geovalues;
    char ***insert_values;
    int geonum;
    int valuesnum;
    size_t gser_length = 0;
    int insert_num = 0;
};

class OGRDMLayer CPL_NON_FINAL : public OGRLayer
{
    OGRDMLayer(const OGRDMLayer &) = delete;
    OGRDMLayer &operator=(const OGRDMLayer &) = delete;

  protected:
    OGRDMFeatureDefn *poFeatureDefn = nullptr;

    int iNextShapeId = 0;
    int iFIDColumn = 0;
    int iGeomColumn = 0;

    static OGRGeometry *BlobToGeometry(const char *);
    static GByte *BlobToGByteArray(const char *pszBlob,
                                   int *pnLength);
    static char *GeometryToBlob(const OGRGeometry *);
    void SetInitialQuery();

    OGRDMDataSource *poDS = nullptr;

    char *pszQueryStatement = nullptr;

    OGRDMStatement *poStatement;
    int nResultOffset = 0;

    char *pszFIDColumn = nullptr;
    char *pszGeomColumn = nullptr;
    int *m_panMapFieldNameToIndex = nullptr;
    int *m_panMapFieldNameToGeomIndex = nullptr;

    virtual CPLString GetFromClauseForGetExtent() = 0;
    OGRErr RunGetExtentRequest(OGREnvelope *psExtent,
                               int bForce,
                               CPLString osCommand,
                               int bErrorAsDebug);
    static void CreateMapFromFieldNameToIndex(OGRDMStatement *hStmt,
                                              OGRFeatureDefn *poFeatureDefn,
                                              int *&panMapFieldNameToIndex,
                                              int *&panMapFieldNameToGeomIndex);

    int ReadResultDefinition(OGRDMStatement *hInitialResultIn);

    OGRFeature *RecordToFeature(OGRDMStatement *hResult,
                                const int *panMapFieldNameToIndex,
                                const int *panMapFieldNameToGeomIndex,
                                int iRecord);

    OGRFeature *GetNextRawFeature();
    OGRDMStatement **stmt = nullptr;
    int col_count;
    ulength rows = 0;
    ulength total_rows = 0;
    char ***result;
    int isfetchall = 0;

  public:
    OGRDMLayer();
    virtual ~OGRDMLayer();

    virtual void ResetReading() override;

    static char *GByteArrayToBlob(const GByte *pabyData,
                                  size_t nLen);
    virtual OGRDMFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    virtual OGRErr GetExtent(int iGeomField,
                             OGREnvelope *psExtent,
                             int bForce) override;
    virtual OGRErr GetExtent(OGREnvelope *psExtent,
                             int bForce) override
    {
        return GetExtent(0, psExtent, bForce);
    }

    virtual const char *GetFIDColumn() override;

    virtual OGRErr SetNextByIndex(GIntBig nIndex) override;

    OGRDMDataSource *GetDS()
    {
        return poDS;
    }

    virtual void ResolveSRID(const OGRDMGeomFieldDefn *poGFldDefn) = 0;
};

class OGRDMTableLayer final : public OGRDMLayer
{
    OGRDMTableLayer(const OGRDMTableLayer &) = delete;
    OGRDMTableLayer &operator=(const OGRDMTableLayer &) = delete;

    int bUpdate = false;

    void BuildWhere();
    CPLString BuildFields();
    void BuildFullQueryStatement();

    char *pszTableName = nullptr;
    char *pszSchemaName = nullptr;
    char *m_pszTableDescription = nullptr;
    CPLString osForcedDescription{};
    char *pszSqlTableName = nullptr;
    int bTableDefinitionValid = -1;

    CPLString osPrimaryKey{};

    int bGeometryInformationSet = false;

    /* Name of the parent table with the geometry definition if it is a derived table or NULL */
    char *pszSqlGeomParentTableName = nullptr;

    char *pszGeomColForced = nullptr;

    CPLString osQuery{};
    CPLString osWHERE{};

    int bLaunderColumnNames = true;
    int bPreservePrecision = true;
    int bCopyActive = false;
    bool bFIDColumnInCopyFields = false;
    int bFirstInsertion = true;

    OGRErr CreateFeatureViaInsert(OGRFeature *poFeature);

    int bHasWarnedIncompatibleGeom = false;
    void CheckGeomTypeCompatibility(int iGeomField,
                                    OGRGeometry *poGeom);

    int bHasWarnedAlreadySetFID = false;

    char **papszOverrideColumnTypes = nullptr;
    int nForcedSRSId = UNDETERMINED_SRID;
    int nForcedGeometryTypeFlags = -1;
    int nForcedCommitCount = 0;
    int nForcedInsert = 0;
    bool bCreateSpatialIndexFlag = true;
    CPLString osSpatialIndexType = "GIST";
    int bInResetReading = false;
    CPLString InsertSQL;
    OGRDMStatement *InsertStatement = nullptr;
    int bAutoFIDOnCreateViaCopy = false;

    int bDeferredCreation = false;
    CPLString osCreateTable{};

    int iFIDAsRegularColumnIndex = -1;

    CPLString m_osFirstGeometryFieldName{};

    std::vector<bool> m_abGeneratedColumns{};
    int checkINI;

    virtual CPLString GetFromClauseForGetExtent() override
    {
        return pszSqlTableName;
    }
    OGRErr RunAddGeometryColumn(const OGRDMGeomFieldDefn *poGeomField);

    CPLErr CheckINI(int *checkini);

  public:
    OGRDMTableLayer(OGRDMDataSource *,
                    CPLString &osCurrentSchema,
                    const char *pszTableName,
                    const char *pszSchemaName,
                    const char *pszDescriptionIn,
                    const char *pszGeomColForced,
                    int bUpdateIn);
    virtual ~OGRDMTableLayer();
    std::map<std::string, int> mymap;
    void SetGeometryInformation(DMGeomColumnDesc *pasDesc,
                                int nGeomFieldCount);

    virtual OGRFeature *GetFeature(GIntBig nFeatureId) override;
    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;
    virtual GIntBig GetFeatureCount(int) override;

    virtual void SetSpatialFilter(OGRGeometry *poGeom) override
    {
        SetSpatialFilter(0, poGeom);
    }
    virtual void SetSpatialFilter(int iGeomField,
                                  OGRGeometry *poGeom) override;

    virtual OGRErr SetAttributeFilter(const char *) override;

    virtual OGRErr ISetFeature(OGRFeature *poFeature) override;
    virtual OGRErr DeleteFeature(GIntBig nFID) override;
    virtual OGRErr ICreateFeature(OGRFeature *poFeature) override;

    virtual OGRErr CreateField(const OGRFieldDefn *poField,
                               int bApproxOK = TRUE) override;
    virtual OGRErr CreateGeomField(const OGRGeomFieldDefn *poGeomField,
                                   int bApproxOK = TRUE) override;
    virtual OGRErr DeleteField(int iField) override;
    virtual OGRErr AlterFieldDefn(int iField,
                                  OGRFieldDefn *poNewFieldDefn,
                                  int nFlags) override;
    virtual int TestCapability(const char *) override;
    virtual OGRErr GetExtent(OGREnvelope *psExtent,
                             int bForce) override
    {
        return GetExtent(0, psExtent, bForce);
    }
    virtual OGRErr GetExtent(int iGeomField,
                             OGREnvelope *psExtent,
                             int bForce) override;

    const char *GetTableName()
    {
        return pszTableName;
    }
    const char *GetSchemaName()
    {
        return pszSchemaName;
    }

    virtual const char *GetFIDColumn() override;

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;
    virtual CPLErr SetMetadata(char **papszMD,
                               const char *pszDomain = "") override;
    virtual CPLErr SetMetadataItem(const char *pszName,
                                   const char *pszValue,
                                   const char *pszDomain = "") override;

    virtual OGRErr Rename(const char *pszNewName) override;

    // follow methods are not base class overrides
    void SetLaunderFlag(int bFlag)
    {
        bLaunderColumnNames = bFlag;
    }
    void SetPrecisionFlag(int bFlag)
    {
        bPreservePrecision = bFlag;
    }

    void SetOverrideColumnTypes(const char *pszOverrideColumnTypes);

    int ReadTableDefinition();
    int HasGeometryInformation()
    {
        return bGeometryInformationSet;
    }
    void SetTableDefinition(const char *pszFIDColumnName,
                            const char *pszGFldName,
                            OGRwkbGeometryType eType,
                            const char *pszGeomType,
                            int nSRSId,
                            int GeometryTypeFlags);

    void SetForcedSRSId(int nForcedSRSIdIn)
    {
        nForcedSRSId = nForcedSRSIdIn;
    }
    void SetForcedGeometryTypeFlags(int GeometryTypeFlagsIn)
    {
        nForcedGeometryTypeFlags = GeometryTypeFlagsIn;
    }
    void SetCreateSpatialIndex(bool bFlag,
                               const char *pszSpatialIndexType)
    {
        bCreateSpatialIndexFlag = bFlag;
        osSpatialIndexType = pszSpatialIndexType;
    }
    void SetForcedDescription(const char *pszDescriptionIn);
    void AllowAutoFIDOnCreateViaCopy()
    {
        bAutoFIDOnCreateViaCopy = TRUE;
    }

    void SetDeferredCreation(CPLString osCreateTable);

    virtual void ResolveSRID(const OGRDMGeomFieldDefn *poGFldDefn) override;
};

class OGRDMResultLayer final : public OGRDMLayer
{
    OGRDMResultLayer(const OGRDMResultLayer &) = delete;
    OGRDMResultLayer &operator=(const OGRDMResultLayer &) = delete;

    void BuildFullQueryStatement();

    char *pszRawStatement = nullptr;

    char *pszGeomTableName = nullptr;
    char *pszGeomTableSchemaName = nullptr;

    CPLString osWHERE{};

    virtual CPLString GetFromClauseForGetExtent() override
    {
        return pszRawStatement;
    }

  public:
    OGRDMResultLayer(OGRDMDataSource *,
                     const char *pszRawStatement,
                     OGRDMStatement *hInitialResult);
    virtual ~OGRDMResultLayer();

    virtual void ResetReading() override;
    virtual GIntBig GetFeatureCount(int) override;

    virtual void SetSpatialFilter(OGRGeometry *poGeom) override
    {
        SetSpatialFilter(0, poGeom);
    }
    virtual void SetSpatialFilter(int iGeomField,
                                  OGRGeometry *poGeom) override;
    virtual int TestCapability(const char *) override;

    virtual OGRFeature *GetNextFeature() override;

    virtual void ResolveSRID(const OGRDMGeomFieldDefn *poGFldDefn) override;
};

class OGRDMDataSource final : public OGRDataSource
{
    OGRDMDataSource(const OGRDMDataSource &) = delete;
    OGRDMDataSource &operator=(const OGRDMDataSource &) = delete;

    typedef struct
    {
        int nMajor;
        int nMinor;
        int nRelease;
    } DMver;

    OGRDMTableLayer **papoLayers = nullptr;
    int nLayers = 0;

    char *pszName = nullptr;

    bool m_bUTF8ClientEncoding = false;

    int bDSUpdate = false;
    int bHaveGeography = true;

    int bUserTransactionActive = false;
    int bSavePointActive = false;
    int nSoftTransactionLevel = 0;

    OGRDMConn *poSession = nullptr;

    OGRErr DeleteLayer(int iLayer) override;

    // We maintain a list of known SRID to reduce the number of trips to
    // the database to get SRSes.
    int nKnownSRID = 0;
    int *panSRID = nullptr;
    OGRSpatialReference **papoSRS = nullptr;

    OGRDMTableLayer *poLayerInCopyMode = nullptr;

    CPLString osCurrentSchema{};

    int nUndefinedSRID = 0;

    char *pszForcedTables = nullptr;
    char **papszSchemaList = nullptr;
    int bHasLoadTables = false;
    CPLString osActiveSchema{};
    int bListAllTables = false;

    CPLString osDebugLastTransactionCommand{};

  public:
    int bUseBinaryCursor = false;
    int bBinaryTimeFormatIsInt8 = false;
    int bUseEscapeStringSyntax = false;

    bool m_bHasGeometryColumns = true;
    bool m_bHasSpatialRefSys = true;

    int GetUndefinedSRID() const
    {
        return nUndefinedSRID;
    }
    bool IsUTF8ClientEncoding() const
    {
        return m_bUTF8ClientEncoding;
    }

  public:
    OGRDMDataSource();
    virtual ~OGRDMDataSource();

    OGRDMConn *GetDMConn()
    {
        return poSession;
    }

    int FetchSRSId(const OGRSpatialReference *poSRS);
    OGRSpatialReference *FetchSRS(int nSRSId);

    int Open(const char *,
             int bUpdate,
             int bTestOpen,
             char **papszOpenOptions);
    OGRDMTableLayer *OpenTable(CPLString &osCurrentSchema,
                               const char *pszTableName,
                               const char *pszSchemaName,
                               const char *pszDescription,
                               const char *pszGeomColForced,
                               int bUpdate,
                               int bTestOpen);

    const char *GetName() override
    {
        return pszName;
    }
    int GetLayerCount() override;
    OGRLayer *GetLayer(int) override;
    OGRLayer *GetLayerByName(const char *pszName) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    int TestCapability(const char *) override;

    virtual OGRLayer *ExecuteSQL(const char *pszSQLCommand,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
    virtual void ReleaseResultSet(OGRLayer *poLayer) override;

    virtual const char *GetMetadataItem(const char *pszKey,
                                        const char *pszDomain) override;
};

OGRDMConn CPL_DLL *OGRGetDMConnection(const char *pszUserid,
                                      const char *pszPassword,
                                      const char *pszDatabase);

CPLString OGRDMCommonLayerGetType(OGRFieldDefn &oField,
                                  bool bPreservePrecision,
                                  bool bApproxOK);

char *strToupper(char *str);

bool OGRDMCommonLayerSetType(OGRFieldDefn &oField,
                             const char *pszType,
                             int nWidth,
                             int sclar);

OGRwkbGeometryType OGRDMCheckType(int typid);

char *OGRDMCommonLaunderName(const char *pszSrcName,
                             const char *pszDebugPrefix = "OGR");

void OGRDMCommonAppendFieldValue(CPLString &osCommand,
                                 OGRFeature *poFeature,
                                 int i);

typedef int lint;
typedef short sint;
typedef signed char tint;
typedef unsigned int ulint;
typedef unsigned short usint;
typedef signed char schar;
typedef unsigned char byte;

typedef usint lwflags_t;

#ifdef WIN32
typedef __int64 lint64;
typedef unsigned __int64 ulint64;
#else
typedef long long int lint64;
typedef unsigned long long int ulint64;
#endif

#define WKB_DOUBLE_SIZE 8
#define WKB_INT_SIZE 4
#define WKB_BYTE_SIZE 1
#define WKB_ISO 0x01
#define WKB_NO_NPOINTS 0x40
#define WKB_NO_SRID 0x80

#define LW_FAILURE 0
#define LW_SUCCESS 1
#define LW_TRUE 1
#define LW_FALSE 0

#define POINTTYPE 1
#define LINETYPE 2
#define POLYGONTYPE 3
#define MULTIPOINTTYPE 4
#define MULTILINETYPE 5
#define MULTIPOLYGONTYPE 6
#define COLLECTIONTYPE 7
#define CIRCSTRINGTYPE 8
#define COMPOUNDTYPE 9
#define CURVEPOLYTYPE 10
#define MULTICURVETYPE 11
#define MULTISURFACETYPE 12
#define POLYHEDRALSURFACETYPE 13
#define TRIANGLETYPE 14
#define TINTYPE 15

#define NUMTYPES 16

#define WKB_POINT_TYPE 1
#define WKB_LINESTRING_TYPE 2
#define WKB_POLYGON_TYPE 3
#define WKB_MULTIPOINT_TYPE 4
#define WKB_MULTILINESTRING_TYPE 5
#define WKB_MULTIPOLYGON_TYPE 6
#define WKB_GEOMETRYCOLLECTION_TYPE 7
#define WKB_CIRCULARSTRING_TYPE 8
#define WKB_COMPOUNDCURVE_TYPE 9
#define WKB_CURVEPOLYGON_TYPE 10
#define WKB_MULTICURVE_TYPE 11
#define WKB_MULTISURFACE_TYPE 12
#define WKB_CURVE_TYPE 13   /* from ISO draft, not sure is real */
#define WKB_SURFACE_TYPE 14 /* from ISO draft, not sure is real */
#define WKB_POLYHEDRALSURFACE_TYPE 15
#define WKB_TIN_TYPE 16
#define WKB_TRIANGLE_TYPE 17

#define WKBZOFFSET 0x80000000
#define WKBMOFFSET 0x40000000
#define WKBSRIDFLAG 0x20000000

#define WKB_EXTENDED 0x04
#define WKB_NDR 0x08
#define WKB_XDR 0x10
#define WKB_HEX 0x20
#define WKB_NO_NPOINTS 0x40

#define SRID_UNKNOWN 0

#define G2FLAG_X_SOLID 0x00000001

#define LWFLAG_Z 0x01
#define LWFLAG_M 0x02
#define LWFLAG_BBOX 0x04
#define LWFLAG_GEODETIC 0x08
#define G2FLAG_X_SOLID 0x00000001
#define LWFLAG_SOLID 0x20
#define LWFLAG_READONLY 0x10
#define G2FLAG_Z 0x01
#define G2FLAG_M 0x02
#define G2FLAG_BBOX 0x04
#define G2FLAG_GEODETIC 0x08
#define G2FLAG_EXTENDED 0x10
#define G2FLAG_RESERVED1 0x20 /* RESERVED FOR FUTURE USES */
#define G2FLAG_VER_0 0x40
#define G2FLAG_RESERVED2 0x80 /* RESERVED FOR FUTURE VERSIONS */
#define GFLAG_VER_0 0x40
#define WKB_EXTENDED 0x04

#define G2FLAGS_GET_EXTENDED(gflags) ((gflags)&G2FLAG_EXTENDED) >> 4
#define G2FLAGS_GET_BBOX(gflags) (((gflags)&G2FLAG_BBOX) >> 2)
#define G2FLAGS_GET_GEODETIC(gflags) (((gflags)&G2FLAG_GEODETIC) >> 3)
#define G2FLAGS_GET_Z(gflags) ((gflags)&G2FLAG_Z)
#define FLAGS_GET_Z(flags) ((flags)&LWFLAG_Z)
#define G2FLAGS_GET_M(gflags) (((gflags)&G2FLAG_M) >> 1)
#define FLAGS_GET_M(flags) (((flags)&LWFLAG_M) >> 1)
#define G2FLAGS_NDIMS(gflags)                                                  \
    (2 + G2FLAGS_GET_Z(gflags) + G2FLAGS_GET_M(gflags))
#define FLAGS_SET_Z(flags, value)                                              \
    ((flags) = (value) ? ((flags) | LWFLAG_Z) : ((flags) & ~LWFLAG_Z))
#define G2FLAGS_GET_Z(gflags) ((gflags)&G2FLAG_Z)
#define FLAGS_SET_M(flags, value)                                              \
    ((flags) = (value) ? ((flags) | LWFLAG_M) : ((flags) & ~LWFLAG_M))
#define G2FLAGS_GET_M(gflags) (((gflags)&G2FLAG_M) >> 1)
#define FLAGS_SET_BBOX(flags, value)                                           \
    ((flags) = (value) ? ((flags) | LWFLAG_BBOX) : ((flags) & ~LWFLAG_BBOX))
#define FLAGS_SET_GEODETIC(flags, value)                                       \
    ((flags) =                                                                 \
         (value) ? ((flags) | LWFLAG_GEODETIC) : ((flags) & ~LWFLAG_GEODETIC))
#define FLAGS_SET_SOLID(flags, value)                                          \
    ((flags) = (value) ? ((flags) | LWFLAG_SOLID) : ((flags) & ~LWFLAG_SOLID))
#define FLAGS_GET_BBOX(flags) (((flags)&LWFLAG_BBOX) >> 2)
#define FLAGS_GET_GEODETIC(flags) (((flags)&LWFLAG_GEODETIC) >> 3)
#define FLAGS_NDIMS(flags) (2 + FLAGS_GET_Z(flags) + FLAGS_GET_M(flags))
#define FLAGS_SET_READONLY(flags, value)                                       \
    ((flags) =                                                                 \
         (value) ? ((flags) | LWFLAG_READONLY) : ((flags) & ~LWFLAG_READONLY))
#define INLINE static

/******************************************************************
* GBOX structure.
* We include the flags (information about dimensionality),
* so we don't have to constantly pass them
* into functions that use the GBOX.
*/
typedef struct
{
    lwflags_t flags;
    double xmin;
    double xmax;
    double ymin;
    double ymax;
    double zmin;
    double zmax;
    double mmin;
    double mmax;
} GBOX;

/******************************************************************
*  POINTARRAY
*  Point array abstracts a lot of the complexity of points and point lists.
*  It handles 2d/3d translation
*    (2d points converted to 3d will have z=0 or NaN)
*  DO NOT MIX 2D and 3D POINTS! EVERYTHING* is either one or the other
*/
typedef struct
{
    ulint npoints; /* how many points we are currently storing */
    ulint maxpoints; /* how many points we have space for in serialized_pointlist */

    /* Use FLAGS_* macros to handle */
    lwflags_t flags;

    /* Array of POINT 2D, 3D or 4D, possibly misaligned. */
    byte *serialized_pointlist;
} POINTARRAY;

/******************************************************************
* LWGEOM (any geometry type)
*
* Abstract type, note that 'type', 'bbox' and 'srid' are available in
* all geometry variants.
*/
typedef struct
{
    GBOX *bbox;
    void *data;
    lint srid;
    lwflags_t flags;
    byte type;
    char pad[1]; /* Padding to 24 bytes (unused) */
} LWGEOM;

/* POINTYPE */
typedef struct
{
    GBOX *bbox;
    POINTARRAY *point; /* hide 2d/3d (this will be an array of 1 point) */
    lint srid;
    lwflags_t flags;
    byte type;   /* POINTTYPE */
    char pad[1]; /* Padding to 24 bytes (unused) */
} LWPOINT;       /* "light-weight point" */

/* LINETYPE */
typedef struct
{
    GBOX *bbox;
    POINTARRAY *points; /* array of POINT3D */
    lint srid;
    lwflags_t flags;
    byte type;   /* LINETYPE */
    char pad[1]; /* Padding to 24 bytes (unused) */
} LWLINE;        /* "light-weight line" */

/* TRIANGLE */
typedef struct
{
    GBOX *bbox;
    POINTARRAY *points;
    lint srid;
    lwflags_t flags;
    byte type;
    char pad[1]; /* Padding to 24 bytes (unused) */
} LWTRIANGLE;

/* CIRCSTRINGTYPE */
typedef struct
{
    GBOX *bbox;
    POINTARRAY *points; /* array of POINT(3D/3DM) */
    lint srid;
    lwflags_t flags;
    byte type;   /* CIRCSTRINGTYPE */
    char pad[1]; /* Padding to 24 bytes (unused) */
} LWCIRCSTRING;  /* "light-weight circularstring" */

/* POLYGONTYPE */
typedef struct
{
    GBOX *bbox;
    POINTARRAY **rings; /* list of rings (list of points) */
    lint srid;
    lwflags_t flags;
    byte type;      /* POLYGONTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint nrings;   /* how many rings we are currently storing */
    ulint maxrings; /* how many rings we have space for in **rings */
} LWPOLY;           /* "light-weight polygon" */

/* MULTIPOINTTYPE */
typedef struct
{
    GBOX *bbox;
    LWPOINT **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* MULTYPOINTTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWMPOINT;

/* MULTILINETYPE */
typedef struct
{
    GBOX *bbox;
    LWLINE **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* MULTILINETYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWMLINE;

/* MULTIPOLYGONTYPE */
typedef struct
{
    GBOX *bbox;
    LWPOLY **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* MULTIPOLYGONTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWMPOLY;

/* COLLECTIONTYPE */
typedef struct
{
    GBOX *bbox;
    LWGEOM **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* COLLECTIONTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWCOLLECTION;

/* COMPOUNDTYPE */
typedef struct
{
    GBOX *bbox;
    LWGEOM **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* COLLECTIONTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWCOMPOUND;       /* "light-weight compound line" */

/* CURVEPOLYTYPE */
typedef struct
{
    GBOX *bbox;
    LWGEOM **rings;
    lint srid;
    lwflags_t flags;
    byte type;      /* CURVEPOLYTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint nrings;   /* how many rings we are currently storing */
    ulint maxrings; /* how many rings we have space for in **rings */
} LWCURVEPOLY;      /* "light-weight polygon" */

/* MULTICURVE */
typedef struct
{
    GBOX *bbox;
    LWGEOM **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* MULTICURVE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWMCURVE;

/* MULTISURFACETYPE */
typedef struct
{
    GBOX *bbox;
    LWGEOM **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* MULTISURFACETYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWMSURFACE;

/* POLYHEDRALSURFACETYPE */
typedef struct
{
    GBOX *bbox;
    LWPOLY **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* POLYHEDRALSURFACETYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWPSURFACE;

/* TINTYPE */
typedef struct
{
    GBOX *bbox;
    LWTRIANGLE **geoms;
    lint srid;
    lwflags_t flags;
    byte type;      /* TINTYPE */
    char pad[1];    /* Padding to 24 bytes (unused) */
    ulint ngeoms;   /* how many geometries we are currently storing */
    ulint maxgeoms; /* how many geometries we have space for in **geoms */
} LWTIN;

/******************************************************************
* POINT2D, POINT3D, POINT3DM, POINT4D
*/
typedef struct
{
    double x, y;
} POINT2D;

typedef struct
{
    double x, y, z;
} POINT3DZ;

typedef struct
{
    double x, y, z;
} POINT3D;

typedef struct
{
    double x, y, m;
} POINT3DM;

typedef struct
{
    double x, y, z, m;
} POINT4D;

#ifdef WORDS_BIGENDIAN
#define LWSIZE_GET(varsize) ((varsize)&0x3FFFFFFF)
#define LWSIZE_SET(varsize, len) ((varsize) = ((len)&0x3FFFFFFF))
#define IS_BIG_ENDIAN 1
#else
#define LWSIZE_GET(varsize) (((varsize) >> 2) & 0x3FFFFFFF)
#define LWSIZE_SET(varsize, len) ((varsize) = (((ulint)(len)) << 2))
#define IS_BIG_ENDIAN 0
#endif

void ptarray_free(POINTARRAY *pa);

void lwpoint_free(LWPOINT *pt);

void lwline_free(LWLINE *line);

void lwpoly_free(LWPOLY *poly);

void lwcircstring_free(LWCIRCSTRING *curve);

void lwtriangle_free(LWTRIANGLE *triangle);

void lwmpoint_free(LWMPOINT *mpt);

void lwmline_free(LWMLINE *mline);

void lwmpoly_free(LWMPOLY *mpoly);

void lwpsurface_free(LWPSURFACE *psurf);

void lwtin_free(LWTIN *tin);

void lwcollection_free(LWCOLLECTION *col);

void lwgeom_free(LWGEOM *lwgeom);

lwflags_t ogr_lwflags(lint hasz,
                      lint hasm,
                      lint geodetic);

LWGEOM *lwgeom_from_gserialized_buffer(byte *data_ptr,
                                       lwflags_t dm_lwflags,
                                       lint srid);

static byte *lwgeom_to_wkb_buf(const LWGEOM *geom,
                               byte *buf,
                               byte variant);

POINTARRAY *ptarray_construct_reference_data(char hasz,
                                             char hasm,
                                             ulint npoints,
                                             byte *ptlist);

POINTARRAY *ptarray_construct_empty(char hasz,
                                    char hasm,
                                    ulint maxpoints);

POINTARRAY *ptarray_construct(char hasz,
                              char hasm,
                              ulint npoints);

LWPOINT *lwpoint_construct(lint srid,
                           GBOX *bbox,
                           POINTARRAY *point);

static int lwgeom_wkb_needs_srid(const LWGEOM *geom,
                                 byte variant);

static ulint64 empty_to_wkb_size(const LWGEOM *geom,
                                 byte variant);

static ulint lwgeom_wkb_type(const LWGEOM *geom,
                             byte variant);

static byte *endian_to_wkb_buf(byte *buf,
                               byte variant);

static int wkb_swap_bytes(byte variant);

static byte *integer_to_wkb_buf(const ulint ival,
                                byte *buf,
                                byte variant);

static byte *double_nan_to_wkb_buf(byte *buf,
                                   byte variant);

static byte *empty_to_wkb_buf(const LWGEOM *geom,
                              byte *buf,
                              byte variant);

static ulint64 ptarray_to_wkb_size(const POINTARRAY *pa,
                                   byte variant);

static byte *get_point_internal(const POINTARRAY *pa,
                                ulint n);

static byte *double_to_wkb_buf(const double d,
                               byte *buf,
                               byte variant);

static byte *ptarray_to_wkb_buf(const POINTARRAY *pa,
                                byte *buf,
                                byte variant);

static LWPOINT *lwpoint_from_gserialized_buffer(byte *data_ptr,
                                                lwflags_t dm_lwflags,
                                                lint srid);

static int lwpoint_is_empty(const LWPOINT *point);

static ulint64 lwpoint_to_wkb_size(const LWPOINT *pt,
                                   byte variant);

static byte *lwpoint_to_wkb_buf(const LWPOINT *pt,
                                byte *buf,
                                byte variant);

static LWLINE *lwline_from_gserialized_buffer(byte *data_ptr,
                                              lwflags_t dm_lwflags,
                                              lint srid);

static int lwline_is_empty(const LWLINE *line);

static ulint64 lwline_to_wkb_size(const LWLINE *line,
                                  byte variant);

static byte *lwline_to_wkb_buf(const LWLINE *line,
                               byte *buf,
                               byte variant);

static LWCIRCSTRING *lwcircstring_from_gserialized_buffer(byte *data_ptr,
                                                          lwflags_t dm_lwflags,
                                                          lint srid);

static int lwcircstring_is_empty(const LWCIRCSTRING *circ);

static int lwtriangle_is_empty(const LWTRIANGLE *triangle);

static ulint64 lwtriangle_to_wkb_size(const LWTRIANGLE *tri,
                                      byte variant);

static byte *lwtriangle_to_wkb_buf(const LWTRIANGLE *tri,
                                   byte *buf,
                                   byte variant);

static LWTRIANGLE *lwtriangle_from_gserialized_buffer(byte *data_ptr,
                                                      lwflags_t dm_lwflags,
                                                      lint srid);

static LWPOLY *lwpoly_from_gserialized_buffer(byte *data_ptr,
                                              lwflags_t dm_lwflags,
                                              ulint64 *size,
                                              lint srid);

static int lwpoly_is_empty(const LWPOLY *poly);

static ulint64 lwpoly_to_wkb_size(const LWPOLY *poly,
                                  byte variant);

static byte *lwpoly_to_wkb_buf(const LWPOLY *poly,
                               byte *buf,
                               byte variant);

int lwcollection_allows_subtype(int collectiontype,
                                int subtype);

static LWCOLLECTION *lwcollection_from_gserialized_buffer(byte *data_ptr,
                                                          lwflags_t dm_lwflags,
                                                          lint srid);

static int lwcollection_is_empty(const LWCOLLECTION *col);

static ulint64 lwcollection_to_wkb_size(const LWCOLLECTION *col,
                                        byte variant);

static byte *lwcompound_to_wkb_buf(const LWCOMPOUND *com,
                                   byte *buf,
                                   byte variant);

static byte *lwcollection_to_wkb_buf(const LWCOLLECTION *col,
                                     byte *buf,
                                     byte variant);

static int lwgeom_is_empty(const LWGEOM *geom);

static ulint64 lwgeom_to_wkb_size(const LWGEOM *geom,
                                  byte variant);

static lint64 lwgeom_to_wkb_write_buf(const LWGEOM *geom,
                                      byte variant,
                                      byte *buffer);

byte *lwgeom_to_wkb_buffer(const LWGEOM *geom,
                           byte variant);

LWGEOM *lwgeom_from_gserialized(GSERIALIZED *geom);

byte *dm_gser_to_wkb(GSERIALIZED *geom,
                     ulint64 *size,
                     ulint *type);

GSERIALIZED *gserialized_from_ewkb(const char *hexwkb,
                                   size_t *size);

#endif  // !OGR_DM_H_INCLUDED
