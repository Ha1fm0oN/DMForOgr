#include "ogr_dm.h"
#include "cpl_conv.h"
#include <ogr_p.h>

CPL_CVSID("$Id$")

OGRDMStatement::OGRDMStatement(OGRDMConn *poConnIn)

{
    poConn = poConnIn;
    hStatement = nullptr;
    result = nullptr;
    papszCurImage = nullptr;
    pszCommandText = nullptr;
    object_index = nullptr;
    objdesc = nullptr;
    obj = nullptr;
    lob = nullptr;
    lob_index = nullptr;
    blob_len = nullptr;
    nRawColumnCount = 0;
    is_fectmany = 0;
    results = nullptr;
    objs = nullptr;
    objdescs = nullptr;
    blob_lens = nullptr;
    papszCurImages = nullptr;
}

OGRDMStatement::~OGRDMStatement()

{
    Clean();
}

void OGRDMStatement::Clean()

{
    DPIRETURN rt;
    if (insert_num > 0)
    {
        rt = dpi_set_stmt_attr(hStatement, DSQL_ATTR_PARAMSET_SIZE,
                               (void *)insert_num, 0);
        rt = dpi_exec(hStatement);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to exectue");
            return;
        }
        insert_num = 0;
        for (int iparam = 0; iparam < geonum; iparam++)
        {
            for (int num = 0; num < insert_num; num++)
            {
                CPLFree(insert_geovalues[iparam][num]);
            }
        }
    }
    rt = dpi_commit((poConn->hCon));
    if (pszCommandText)
        CPLFree(pszCommandText);
    pszCommandText = nullptr;
    if (is_fectmany == 0)
    {
        if (result)
        {
            for (int i = 0; result[i] != nullptr; i++)
            {
                CPLFree(result[i]);
                if (object_index[i])
                {
                    rt = dpi_free_obj(obj[i]);
                    if (!DSQL_SUCCEEDED(rt))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "failed to free obj");
                    }
                    dpi_free_obj_desc(objdesc[i]);
                    if (!DSQL_SUCCEEDED(rt))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "failed to free objdesc");
                    }
                }
                else if (lob_index[i])
                {
                    rt = dpi_free_lob_locator(lob[i]);
                    if (!DSQL_SUCCEEDED(rt))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "failed to free lob");
                    }
                }
            }
            CPLFree(result);
            CPLFree(obj);
            CPLFree(objdesc);
            CPLFree(lob);
            CPLFree(blob_len);
        }
        if (papszCurImage)
            CPLFree(papszCurImage);
    }
    else
    {
        if (results)
        {
            for (int col = 0; results[col] != nullptr; col++)
            {

                if (object_index[col])
                {
                    for (int row = 0; row < fetchnum; row++)
                    {
                        if (results[col][row])
                            CPLFree(results[col][row]);
                        rt = dpi_free_obj(objs[col][row]);
                        if (!DSQL_SUCCEEDED(rt))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "failed to free obj");
                        }
                        dpi_free_obj_desc(objdescs[col][row]);
                        if (!DSQL_SUCCEEDED(rt))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "failed to free objdesc");
                        }
                    }
                }
                else if (lob_index[col])
                {
                    for (int row = 0; row < fetchnum; row++)
                    {
                        if (results[col][row])
                            CPLFree(results[col][row]);
                        rt = dpi_free_lob_locator(lobs[col][row]);
                        if (!DSQL_SUCCEEDED(rt))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "failed to free lob");
                        }
                    }
                }
                else
                {
                    if (results[col][0])
                        CPLFree(results[col][0]);
                }
                CPLFree(results[col]);
                CPLFree(objs[col]);
                CPLFree(objdescs[col]);
                CPLFree(lobs[col]);
                CPLFree(blob_lens[col]);
                if (papszCurImages && papszCurImages[col])
                    CPLFree(papszCurImages[col]);
            }
            CPLFree(results);
            CPLFree(objs);
            CPLFree(objdescs);
            CPLFree(lobs);
            CPLFree(blob_lens);
            if (papszCurImages)
                CPLFree(papszCurImages);
        }
    }
    if (object_index)
        CPLFree(object_index);
    if (lob_index)
        CPLFree(lob_index);
    object_index = nullptr;
    objdesc = nullptr;
    obj = nullptr;
    lob = nullptr;
    lob_index = nullptr;
    blob_len = nullptr;
    nRawColumnCount = 0;
    is_fectmany = 0;
    results = nullptr;
    objs = nullptr;
    objdescs = nullptr;
    blob_lens = nullptr;
    papszCurImages = nullptr;
    papszCurImage = nullptr;

    if (hStatement)
    {
        rt = dpi_free_stmt(hStatement);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to free stmt");
        }
        hStatement = nullptr;
    }
}

CPLErr OGRDMStatement::Prepare(const char *pszSQLstatement)

{
    DPIRETURN rt;
    Clean();

    CPLDebug("DM", "Prepare(%s)", pszSQLstatement);

    pszCommandText = CPLStrdup(pszSQLstatement);

    if (hStatement != nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Statement already executed once on this OGRDMStatement.");
        return CE_Failure;
    }

    rt = dpi_alloc_stmt((poConn->hCon), &hStatement);

    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to alloc statement");
        return CE_Failure;
    }

    rt = dpi_prepare(hStatement, (sdbyte *)pszCommandText);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "failed to prepare, %s",
                 pszSQLstatement);
        return CE_Failure;
    }
    return CE_None;
}

CPLErr OGRDMStatement::Execute_for_insert(OGRDMFeatureDefn *params,
                                          OGRFeature *poFeature,
                                          std::map<std::string, int> mymap)
{
    DPIRETURN rt;
    int i = 0;
    int bind_flag = 0;
    if (paramdescs == nullptr)
    {
        bind_flag = 1;
        rt = dpi_set_stmt_attr(hStatement, DSQL_ATTR_PARAMSET_SIZE,
                               (void *)FORCED_INSERT_NUM, 0);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to set stmt paramset size");
            return CE_Failure;
        }

        rt = dpi_number_params(hStatement, (sdint2 *)&param_nums);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get params numbers");
            return CE_Failure;
        }
        paramdescs = (DmColDesc *)CPLCalloc(sizeof(DmColDesc), param_nums);
        for (udint2 iparam = 0; iparam < param_nums; iparam++)
        {
            rt = dpi_desc_param(
                hStatement, iparam + 1, &paramdescs[iparam].sql_type,
                &paramdescs[iparam].prec, &paramdescs[iparam].scale,
                &paramdescs[iparam].nullable);
            if (paramdescs[iparam].sql_type == DSQL_CLASS)
                i++;
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get param desc");
                return CE_Failure;
            }
        }

        geonum = i;
        insert_objs = (dhobj **)CPLCalloc(sizeof(dhobj *), i);
        for (int num = 0; num < i; num++)
        {
            insert_objs[num] =
                (dhobj *)CPLCalloc(sizeof(dhobj), FORCED_INSERT_NUM);
        }
        if (i > 0)
        {
            dhdesc hdesc_param;
            sdint4 val_len;
            rt = dpi_get_stmt_attr(hStatement, DSQL_ATTR_IMP_PARAM_DESC,
                                   (dpointer)&hdesc_param, 0, &val_len);
            rt = dpi_get_desc_field(hdesc_param, (sdint2)1,
                                    DSQL_DESC_OBJ_DESCRIPTOR, &insert_objdesc,
                                    sizeof(dhobjdesc), NULL);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get geometry desc");
                return CE_Failure;
            }
        }
        for (int iparam = 0; iparam < i; iparam++)
        {
            for (int num = 0; num < FORCED_INSERT_NUM; num++)
            {
                rt = dpi_alloc_obj((poConn->hCon), &insert_objs[iparam][num]);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to alloc obj");
                    return CE_Failure;
                }
                rt =
                    dpi_bind_obj_desc(insert_objs[iparam][num], insert_objdesc);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to bind obj desc");
                    return CE_Failure;
                }
            }
        }
        insert_geovalues =
            (GSERIALIZED ***)CPLCalloc(sizeof(GSERIALIZED **), i);
        for (int iparam = 0; iparam < i; iparam++)
        {
            insert_geovalues[iparam] = (GSERIALIZED **)CPLCalloc(
                sizeof(GSERIALIZED *), FORCED_INSERT_NUM);
        }

        valuesnum = param_nums - i;
        insert_values = (char ***)CPLCalloc(sizeof(char **), valuesnum);
        for (int iparam = 0; iparam < valuesnum; iparam++)
        {
            insert_values[iparam] =
                (char **)CPLCalloc(sizeof(char *), FORCED_INSERT_NUM);
            char *date = (char *)CPLMalloc(8192 * FORCED_INSERT_NUM);
            for (int num = 0; num < FORCED_INSERT_NUM; num++)
            {
                insert_values[iparam][num] = date + 8192 * num;
            }
        }
    }
    i = 0;
    for (udint2 num = 0; num < geonum; num++)
    {

        OGRDMGeomFieldDefn *poGeomFieldDefn = params->GetGeomFieldDefn(i);
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        char s[100];
        strncpy(s, poGeomFieldDefn->GetNameRef(), 100);
        if (mymap[strToupper(s)] == num + 1 && poGeom != nullptr)
        {
            if (i < params->GetGeomFieldCount())
                i++;
            if (poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOGRAPHY ||
                poGeomFieldDefn->eDMGeoType == GEOM_TYPE_GEOMETRY)
            {
                poGeom->closeRings();
                poGeom->set3D(poGeomFieldDefn->GeometryTypeFlags &
                              OGRGeometry::OGR_G_3D);
                poGeom->setMeasured(poGeomFieldDefn->GeometryTypeFlags &
                                    OGRGeometry::OGR_G_MEASURED);

                int nSRSId = poGeomFieldDefn->nSRSId;

                if (!CPLTestBool(CPLGetConfigOption("DM_USE_TEXT", "NO")))
                {
                    char *pszHexEWKB =
                        OGRGeometryToHexEWKB(poGeom, nSRSId, 3, 3);
                    insert_geovalues[num][insert_num] =
                        gserialized_from_ewkb(pszHexEWKB, &gser_length);
                    CPLFree(pszHexEWKB);
                    rt = dpi_set_obj_val(
                        insert_objs[num][insert_num], 1, DSQL_C_BINARY,
                        insert_geovalues[num][insert_num], gser_length);
                    if (!DSQL_SUCCEEDED(rt))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "failed to set obj val");
                        return CE_Failure;
                    }
                    if (bind_flag == 1)
                    {
                        rt = dpi_bind_param(
                            hStatement, num + 1, DSQL_PARAM_INPUT, DSQL_C_CLASS,
                            DSQL_CLASS, paramdescs[num].prec,
                            paramdescs[num].scale, &insert_objs[num][0],
                            sizeof(dhobj), NULL);
                        if (!DSQL_SUCCEEDED(rt))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "failed to bind param");
                            return CE_Failure;
                        }
                    }
                }
            }
        }
        else
        {
            rt = dpi_set_obj_val(insert_objs[num][insert_num], 1, DSQL_C_BINARY,
                                 nullptr, 0);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to set obj val");
                return CE_Failure;
            }
            if (bind_flag == 1)
            {
                rt = dpi_bind_param(hStatement, num + 1, DSQL_PARAM_INPUT,
                                    DSQL_C_CLASS, DSQL_CLASS,
                                    paramdescs[num].prec, paramdescs[num].scale,
                                    &insert_objs[num][0], sizeof(dhobj), NULL);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to bind param");
                    return CE_Failure;
                }
            }
        }
    }
    i = 0;
    for (int num = 0; num < valuesnum; num++)
    {

        OGRFeatureDefn *poFeatureDefn = poFeature->GetDefnRef();
        OGRFieldType nOGRFieldType = poFeatureDefn->GetFieldDefn(i)->GetType();
        char s[100];
        strncpy(s, poFeatureDefn->GetFieldDefn(i)->GetNameRef(), 100);
        if (mymap[strToupper(s)] == num + geonum + 1)
        {

            strncpy(insert_values[num][insert_num],
                    poFeature->GetFieldAsString(i), 8192);
            if (i < params->GetFieldCount())
                i++;
            // Check if date is NULL: 0000-00-00
            if (nOGRFieldType == OFTDate)
            {
                if (STARTS_WITH_CI(insert_values[num][insert_num], "0000"))
                {
                    strncpy(insert_values[num][insert_num], "NULL", 8192);
                }
            }
            else if (nOGRFieldType == OFTReal)
            {
                //Check for special values. They need to be quoted.
                double dfVal = poFeature->GetFieldAsDouble(i);
                if (CPLIsNan(dfVal))
                    strncpy(insert_values[num][insert_num], "'NaN'", 8192);
            }
            if (bind_flag == 1)
            {
                rt = dpi_bind_param(hStatement, (udint2)(num + geonum + 1),
                                    DSQL_PARAM_INPUT, DSQL_C_NCHAR,
                                    paramdescs[num + geonum].sql_type,
                                    paramdescs[num + geonum].prec,
                                    paramdescs[num + geonum].scale,
                                    insert_values[num][0], 8192, NULL);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to bind param");
                    return CE_Failure;
                }
            }
        }
        else
        {
            strncpy(insert_values[num][insert_num], "", 8192);
            if (bind_flag == 1)
            {
                rt = dpi_bind_param(hStatement, (udint2)(num + geonum + 1),
                                    DSQL_PARAM_INPUT, DSQL_C_NCHAR,
                                    paramdescs[num + geonum].sql_type,
                                    paramdescs[num + geonum].prec,
                                    paramdescs[num + geonum].scale,
                                    insert_values[num][0], 8192, NULL);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to bind param");
                    return CE_Failure;
                }
            }
        }
    }
    insert_num++;
    if (insert_num < FORCED_INSERT_NUM)
        return CE_None;
    rt = dpi_exec(hStatement);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to exectue");
        return CE_Failure;
    }
    rt = dpi_commit((poConn->hCon));
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to commit");
        return CE_Failure;
    }
    insert_num = 0;
    for (int iparam = 0; iparam < geonum; iparam++)
    {
        for (int num = 0; num < FORCED_INSERT_NUM; num++)
        {
            CPLFree(insert_geovalues[iparam][num]);
        }
    }

    return CE_None;
}

CPLErr OGRDMStatement::ExecuteInsert(const char *pszSQLStatement, int nMode)
{
    DPIRETURN rt;

    rt = dpi_alloc_stmt((poConn->hCon), &hStatement);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to alloc statement");
        return CE_Failure;
    }
    rt = dpi_exec_direct(hStatement, (sdbyte *)pszSQLStatement);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to exectue");
        return CE_Failure;
    }

    sdint8 row_count;
    rt = dpi_row_count(hStatement, &row_count);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get row_count");
        return CE_Failure;
    }
    return CE_None;
}

CPLErr OGRDMStatement::Execute(const char *pszSQLStatement, int nMode)
{
    DPIRETURN rt;

    if (pszSQLStatement != nullptr)
    {
        CPLErr eErr = Prepare(pszSQLStatement);
        if (eErr != CE_None)
            return eErr;
    }

    if (hStatement == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "prepare null");
        return CE_Failure;
    }

    sdint4 nStmtType;
    slength len;
    rt = dpi_get_diag_field(DSQL_HANDLE_STMT, hStatement, 0,
                            DSQL_DIAG_DYNAMIC_FUNCTION_CODE,
                            (dpointer)&nStmtType, 0, &len);

    int bSelect = (nStmtType == DSQL_DIAG_FUNC_CODE_SELECT);

    rt = dpi_exec(hStatement);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to exectue");
        return CE_Failure;
    }

    if (!bSelect)
    {
        sdint8 row_count;
        rt = dpi_row_count(hStatement, &row_count);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get row_count");
            return CE_Failure;
        }
        return CE_None;
    }

    sdint2 column_count;
    rt = dpi_number_columns(hStatement, &column_count);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get columns_count");
        return CE_Failure;
    }
    nRawColumnCount = column_count;
    object_index = (int *)CPLCalloc(sizeof(int), column_count);
    lob_index = (int *)CPLCalloc(sizeof(int), column_count);
    lob = (dhloblctr *)CPLCalloc(sizeof(dhloblctr), column_count);
    obj = (dhobj *)CPLCalloc(sizeof(dhobj), column_count);
    objdesc = (dhobjdesc *)CPLCalloc(sizeof(dhobjdesc), column_count);
    blob_len = (int *)CPLCalloc(sizeof(int), column_count);
    result = (char **)CPLCalloc(sizeof(char *), nRawColumnCount + 1);

    dhdesc hdesc_col;
    sdint4 val_len;
    rt = dpi_get_stmt_attr(hStatement, DSQL_ATTR_IMP_ROW_DESC,
                           (dpointer)&hdesc_col, 0, &val_len);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get row_desc");
        return CE_Failure;
    }

    for (int iParam = 0; iParam < nRawColumnCount; iParam++)
    {
        DmColDesc coldesc;
        rt = dpi_desc_column(hStatement, (sdint2)iParam + 1, coldesc.name,
                             sizeof(coldesc.name), &coldesc.nameLen,
                             &coldesc.sql_type, &coldesc.prec, &coldesc.scale,
                             &coldesc.nullable);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get columns_desc");
            return CE_Failure;
        }

        if (coldesc.sql_type == DSQL_CLASS)
        {

            rt = dpi_get_desc_field(
                hdesc_col, (sdint2)iParam + 1, DSQL_DESC_OBJ_DESCRIPTOR,
                (dpointer)&objdesc[iParam], sizeof(dhobjdesc), NULL);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get object descriptor");
                return CE_Failure;
            }
            rt = dpi_alloc_obj((poConn->hCon), &obj[iParam]);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to alloc obj");
                return CE_Failure;
            }
            rt = dpi_bind_obj_desc(obj[iParam], objdesc[iParam]);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to bind obj");
                return CE_Failure;
            }
            rt = dpi_bind_col(hStatement, (udint2)iParam + 1, DSQL_C_CLASS,
                              &obj[iParam], sizeof(obj[iParam]), NULL);
            object_index[iParam] = 1;
            lob_index[iParam] = 0;
        }
        else if (coldesc.sql_type == DSQL_BLOB || coldesc.sql_type == DSQL_CLOB)
        {
            rt = dpi_alloc_lob_locator(hStatement, &lob[iParam]);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to alloc lob");
                return CE_Failure;
            }
            rt = dpi_bind_col(hStatement, (udint2)iParam + 1, DSQL_C_LOB_HANDLE,
                              &lob[iParam], 0, NULL);
            if (coldesc.sql_type == DSQL_BLOB)
                lob_index[iParam] = 2;
            else
                lob_index[iParam] = 1;
            object_index[iParam] = 0;
        }
        else
        {
            rt = dpi_get_desc_field(
                hdesc_col, (sdint2)iParam + 1, DSQL_DESC_DISPLAY_SIZE,
                (dpointer)&coldesc.display_size, 0, &val_len);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get col display_size");
                return CE_Failure;
            }

            int nbufwidth = 256;
            if (coldesc.prec > 0)
                nbufwidth = (int)coldesc.display_size + 3;
            result[iParam] = (char *)CPLMalloc(nbufwidth + 2);

            rt = dpi_bind_col(hStatement, (udint2)iParam + 1, DSQL_C_NCHAR,
                              (dpointer)result[iParam],
                              coldesc.display_size + 1, NULL);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to bind col");
                return CE_Failure;
            }
            object_index[iParam] = 0;
            lob_index[iParam] = 0;
        }
    }
    return CE_None;
}

CPLErr OGRDMStatement::Excute_for_fetchmany(const char *pszSQLStatement)
{
    DPIRETURN rt;

    if (pszSQLStatement != nullptr)
    {
        CPLErr eErr = Prepare(pszSQLStatement);
        if (eErr != CE_None)
            return eErr;
    }

    if (hStatement == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "prepare null");
        return CE_Failure;
    }

    rt = dpi_set_stmt_attr(hStatement, DSQL_ATTR_ROW_ARRAY_SIZE,
                           (void *)fetchnum, 0);
    sdint4 nStmtType;
    slength len;
    rt = dpi_get_diag_field(DSQL_HANDLE_STMT, hStatement, 0,
                            DSQL_DIAG_DYNAMIC_FUNCTION_CODE,
                            (dpointer)&nStmtType, 0, &len);

    int bSelect = (nStmtType == DSQL_DIAG_FUNC_CODE_SELECT);

    rt = dpi_exec(hStatement);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to exectue");
        return CE_Failure;
    }

    if (!bSelect)
    {
        sdint8 row_count;
        rt = dpi_row_count(hStatement, &row_count);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get row_count");
            return CE_Failure;
        }
        return CE_None;
    }
    is_fectmany = 1;
    sdint2 column_count;
    rt = dpi_number_columns(hStatement, &column_count);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get columns_count");
        return CE_Failure;
    }
    nRawColumnCount = column_count;
    object_index = (int *)CPLCalloc(sizeof(int), column_count);
    lob_index = (int *)CPLCalloc(sizeof(int), column_count);
    results = (char ***)CPLCalloc(sizeof(char **), column_count + 1);
    lobs = (dhloblctr **)CPLCalloc(sizeof(dhloblctr *), column_count);
    objs = (dhobj **)CPLCalloc(sizeof(dhobj *), column_count);
    blob_lens = (int **)CPLCalloc(sizeof(int *), column_count);
    objdescs = (dhobjdesc **)CPLCalloc(sizeof(dhobjdesc *), column_count);
    for (int i = 0; i < column_count; i++)
    {
        results[i] = (char **)CPLCalloc(sizeof(char *), fetchnum);
        blob_lens[i] = (int *)CPLCalloc(sizeof(int), fetchnum);
        lobs[i] = (dhloblctr *)CPLCalloc(sizeof(dhloblctr), fetchnum);
        objs[i] = (dhobj *)CPLCalloc(sizeof(dhobj), fetchnum);
        objdescs[i] = (dhobjdesc *)CPLCalloc(sizeof(dhobjdesc), fetchnum);
    }

    dhdesc hdesc_col;
    sdint4 val_len;
    rt = dpi_get_stmt_attr(hStatement, DSQL_ATTR_IMP_ROW_DESC,
                           (dpointer)&hdesc_col, 0, &val_len);
    if (!DSQL_SUCCEEDED(rt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "failed to get row_desc");
        return CE_Failure;
    }

    for (int iParam = 0; iParam < nRawColumnCount; iParam++)
    {
        DmColDesc coldesc;
        rt = dpi_desc_column(hStatement, (sdint2)iParam + 1, coldesc.name,
                             sizeof(coldesc.name), &coldesc.nameLen,
                             &coldesc.sql_type, &coldesc.prec, &coldesc.scale,
                             &coldesc.nullable);
        if (!DSQL_SUCCEEDED(rt))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "failed to get columns_desc");
            return CE_Failure;
        }

        if (coldesc.sql_type == DSQL_CLASS)
        {
            rt = dpi_get_desc_field(
                hdesc_col, (sdint2)iParam + 1, DSQL_DESC_OBJ_DESCRIPTOR,
                &(objdescs[iParam][0]), sizeof(dhobjdesc), NULL);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get object descriptor");
                return CE_Failure;
            }
            for (int i = 0; i < fetchnum; i++)
            {
                rt = dpi_alloc_obj((poConn->hCon), &(objs[iParam][i]));

                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to alloc obj");
                    return CE_Failure;
                }
                rt = dpi_bind_obj_desc(objs[iParam][i], objdescs[iParam][0]);

                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to bind obj");
                    return CE_Failure;
                }
            }

            rt = dpi_bind_col(hStatement, (udint2)iParam + 1, DSQL_C_CLASS,
                              &objs[iParam][0], sizeof(objs[iParam][0]), NULL);

            object_index[iParam] = 1;
            lob_index[iParam] = 0;
        }
        else if (coldesc.sql_type == DSQL_BLOB || coldesc.sql_type == DSQL_CLOB)
        {
            for (int i = 0; i < fetchnum; i++)
            {
                rt = dpi_alloc_lob_locator(hStatement, &(lobs[iParam][i]));
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to alloc lob");
                    return CE_Failure;
                }
            }
            rt = dpi_bind_col(hStatement, (udint2)iParam + 1, DSQL_C_LOB_HANDLE,
                              &lobs[iParam][0], sizeof(lobs[iParam][0]), NULL);
            if (coldesc.sql_type == DSQL_BLOB)
                lob_index[iParam] = 2;
            else
                lob_index[iParam] = 1;
            object_index[iParam] = 0;
        }
        else
        {
            rt = dpi_get_desc_field(
                hdesc_col, (sdint2)iParam + 1, DSQL_DESC_DISPLAY_SIZE,
                (dpointer)&coldesc.display_size, 0, &val_len);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to get col display_size");
                return CE_Failure;
            }

            int nbufwidth = 256;

            if (coldesc.prec > 0)
                nbufwidth = (int)coldesc.display_size + 3;
            char *date = (char *)CPLMalloc((nbufwidth + 2) * fetchnum);
            for (int i = 0; i < fetchnum; i++)
            {
                //results[i][iParam] = (char*)CPLMalloc(nbufwidth + 2);
                results[iParam][i] = date + i * (nbufwidth + 2);
            }
            rt = dpi_bind_col(
                hStatement, (udint2)iParam + 1, DSQL_C_NCHAR,
                (dpointer)results[iParam][0] /*results[0][iParam]*/,
                nbufwidth + 2, NULL);
            if (!DSQL_SUCCEEDED(rt))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "failed to bind col");
                return CE_Failure;
            }
            object_index[iParam] = 0;
            lob_index[iParam] = 0;
        }
    }
    return CE_None;
}

char **OGRDMStatement::SimpleFetchRow()
{
    int i;
    DPIRETURN rt;
    if (papszCurImage == nullptr)
    {
        papszCurImage = (char **)CPLCalloc(sizeof(char *), nRawColumnCount + 1);
    }
    ulength rows;
    rt = dpi_fetch(hStatement, &rows);
    if (rt == DSQL_NO_DATA)
        return nullptr;

    for (i = 0; i < nRawColumnCount; i++)
    {
        if (object_index[i] == 0 && lob_index[i] == 0)
            papszCurImage[i] = result[i];
    }

    return papszCurImage;
}

char ***OGRDMStatement::Fetchmany(ulength *rows)
{
    DPIRETURN rt = 0;
    ulength row = 0;

    rt = dpi_fetch(hStatement, &row);
    if (!DSQL_SUCCEEDED(rt))
        return nullptr;

    *rows = row;

    if (papszCurImages == nullptr)
    {
        papszCurImages =
            (char ***)CPLCalloc(sizeof(char **), nRawColumnCount + 1);
        for (int i = 0; i < nRawColumnCount; i++)
        {
            papszCurImages[i] = (char **)CPLCalloc(sizeof(char *), fetchnum);
        }
    }

    for (ulength i = 0; i < nRawColumnCount; i++)
    {
        if (object_index[i] == 0 && lob_index[i] == 0)
        {
            for (int num = 0; num < *rows; num++)
            {
                papszCurImages[i][num] = results[i][num];
            }
        }
        else if (object_index[i] == 1)
        {
            slength real_len, val_len;
            for (int num = 0; num < *rows; num++)
            {
                rt = dpi_get_obj_val((dhobj)objs[i][num], 1, DSQL_C_BINARY,
                                     NULL, 0, &real_len);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Debug, CPLE_AppDefined,
                             "failed to get object len or object is empty");
                    if (results[i][num])
                    {
                        CPLFree(results[i][num]);
                    }
                    results[i][num] = nullptr;
                    papszCurImages[i][num] = nullptr;
                    continue;
                }
                if (!results[i][num])
                {
                    results[i][num] = (char *)CPLMalloc(1000);
                }
                if (real_len > 1000)
                {
                    CPLFree(results[i][num]);
                    results[i][num] = (char *)CPLMalloc(real_len);
                }
                else
                {
                    real_len = 1000;
                }
                rt = dpi_get_obj_val((dhobj)objs[i][num], 1, DSQL_C_BINARY,
                                     results[i][num], (udint4)real_len,
                                     &val_len);
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to get object value");
                    return nullptr;
                }
                papszCurImages[i][num] = results[i][num];
            }
        }
        else
        {
            slength real_len, val_len;
            for (int num = 0; num < *rows; num++)
            {
                rt = dpi_lob_get_length((dhloblctr)lobs[i][num], &real_len);
                if (!DSQL_SUCCEEDED(rt) || real_len == -1)
                {
                    CPLError(CE_Debug, CPLE_AppDefined,
                             "failed to get lob len or lob is empty");
                    if (results[i][num])
                    {
                        CPLFree(results[i][num]);
                    }
                    results[i][num] = nullptr;
                    papszCurImages[i][num] = nullptr;
                    continue;
                }
                if (results[i][num])
                {
                    CPLFree(results[i][num]);
                }
                char *objvalue = (char *)CPLMalloc(real_len + 3);
                if (lob_index[i] == 2)
                {
                    rt = dpi_lob_read((dhloblctr)lobs[i][num], 1, DSQL_C_BINARY,
                                      0, objvalue, real_len + 1, &val_len);
                    blob_lens[i][num] = (int)val_len;
                }
                else
                {
                    rt = dpi_lob_read((dhloblctr)lobs[i][num], 1, DSQL_C_NCHAR,
                                      0, objvalue, real_len + 1, &val_len);
                }
                if (!DSQL_SUCCEEDED(rt))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "failed to get object value");
                    return nullptr;
                }
                results[i][num] = objvalue;
                papszCurImages[i][num] = results[i][num];
            }
        }
    }

    return papszCurImages;
}
