#include "ogr_dm.h"
#include <math.h>
#include <limits.h>
#include <float.h>

/** Max depth in a geometry. Matches the default YYINITDEPTH for WKT */
#define LW_PARSER_MAX_DEPTH 200

/**
 * Parser check flags
 *
 *  @see lwgeom_from_wkb
 *  @see lwgeom_from_hexwkb
 *  @see lwgeom_parse_wkt
 */
#define LW_PARSER_CHECK_MINPOINTS 1
#define LW_PARSER_CHECK_ODD 2
#define LW_PARSER_CHECK_CLOSURE 4
#define LW_PARSER_CHECK_ZCLOSURE 8

#define SRID_UNKNOWN 0
#define SRID_MAXIMUM 999999
#define SRID_USER_MAXIMUM 998999

#define EPSILON_SQLMM 1e-8
#define FP_TOLERANCE 1e-12
#define FP_EQUALS(A, B) (fabs((A) - (B)) <= FP_TOLERANCE)
#define FP_IS_ZERO(A) (fabs(A) <= FP_TOLERANCE)

#define SIGNUM(n) (((n) > 0) - ((n) < 0))

#define LWFLAG_Z 0x01
#define LWFLAG_M 0x02
#define LWFLAG_BBOX 0x04
#define LWFLAG_GEODETIC 0x08
#define LWFLAG_READONLY 0x10
#define LWFLAG_SOLID 0x20

#define FLAGS_GET_Z(flags) ((flags)&LWFLAG_Z)
#define FLAGS_GET_M(flags) (((flags)&LWFLAG_M) >> 1)
#define FLAGS_GET_BBOX(flags) (((flags)&LWFLAG_BBOX) >> 2)
#define FLAGS_GET_GEODETIC(flags) (((flags)&LWFLAG_GEODETIC) >> 3)
#define FLAGS_GET_READONLY(flags) (((flags)&LWFLAG_READONLY) >> 4)
#define FLAGS_GET_SOLID(flags) (((flags)&LWFLAG_SOLID) >> 5)

#define FLAGS_SET_Z(flags, value)                                              \
    ((flags) = (value) ? ((flags) | LWFLAG_Z) : ((flags) & ~LWFLAG_Z))
#define FLAGS_SET_M(flags, value)                                              \
    ((flags) = (value) ? ((flags) | LWFLAG_M) : ((flags) & ~LWFLAG_M))
#define FLAGS_SET_BBOX(flags, value)                                           \
    ((flags) = (value) ? ((flags) | LWFLAG_BBOX) : ((flags) & ~LWFLAG_BBOX))
#define FLAGS_SET_GEODETIC(flags, value)                                       \
    ((flags) =                                                                 \
         (value) ? ((flags) | LWFLAG_GEODETIC) : ((flags) & ~LWFLAG_GEODETIC))
#define FLAGS_SET_READONLY(flags, value)                                       \
    ((flags) =                                                                 \
         (value) ? ((flags) | LWFLAG_READONLY) : ((flags) & ~LWFLAG_READONLY))
#define FLAGS_SET_SOLID(flags, value)                                          \
    ((flags) = (value) ? ((flags) | LWFLAG_SOLID) : ((flags) & ~LWFLAG_SOLID))

#define FLAGS_NDIMS(flags) (2 + FLAGS_GET_Z(flags) + FLAGS_GET_M(flags))
#define FLAGS_GET_ZM(flags) (FLAGS_GET_M(flags) + FLAGS_GET_Z(flags) * 2)
#define FLAGS_NDIMS_BOX(flags)                                                 \
    (FLAGS_GET_GEODETIC(flags) ? 3 : FLAGS_NDIMS(flags))

#define G2FLAGS_SET_Z(gflags, value)                                           \
    ((gflags) = (value) ? ((gflags) | G2FLAG_Z) : ((gflags) & ~G2FLAG_Z))
#define G2FLAGS_SET_M(gflags, value)                                           \
    ((gflags) = (value) ? ((gflags) | G2FLAG_M) : ((gflags) & ~G2FLAG_M))
#define G2FLAGS_SET_BBOX(gflags, value)                                        \
    ((gflags) = (value) ? ((gflags) | G2FLAG_BBOX) : ((gflags) & ~G2FLAG_BBOX))
#define G2FLAGS_SET_GEODETIC(gflags, value)                                    \
    ((gflags) = (value) ? ((gflags) | G2FLAG_GEODETIC)                         \
                        : ((gflags) & ~G2FLAG_GEODETIC))
#define G2FLAGS_SET_EXTENDED(gflags, value)                                    \
    ((gflags) = (value) ? ((gflags) | G2FLAG_EXTENDED)                         \
                        : ((gflags) & ~G2FLAG_EXTENDED))
#define G2FLAGS_SET_VERSION(gflags, value)                                     \
    ((gflags) =                                                                \
         (value) ? ((gflags) | G2FLAG_VER_0) : ((gflags) & ~G2FLAG_VER_0))

#define LW_PARSER_CHECK_MINPOINTS 1
#define LW_PARSER_CHECK_ODD 2
#define LW_PARSER_CHECK_CLOSURE 4
#define LW_PARSER_CHECK_ZCLOSURE 8

#define LW_PARSER_CHECK_NONE 0
#define LW_PARSER_CHECK_ALL                                                    \
    (LW_PARSER_CHECK_MINPOINTS | LW_PARSER_CHECK_ODD | LW_PARSER_CHECK_CLOSURE)

#define FP_MAX(A, B) (((A) > (B)) ? (A) : (B))
#define FP_MIN(A, B) (((A) < (B)) ? (A) : (B))

#define NO_VALUE 0.0
#define NO_Z_VALUE NO_VALUE
#define NO_M_VALUE NO_VALUE

/**
* Used for passing the parse state between the parsing functions.
*/
typedef struct
{
    const unsigned char *wkb; /* Points to start of WKB */
    int srid;                 /* Current SRID we are handling */
    size_t wkb_size;          /* Expected size of WKB */
    signed char swap_bytes;   /* Do an endian flip? */
    signed char check;        /* Simple validity checks on geometries */
    signed char lwtype;       /* Current type we are handling */
    signed char has_z;        /* Z? */
    signed char has_m;        /* M? */
    signed char has_srid;     /* SRID? */
    signed char error; /* An error was found (not enough bytes to read) */
    unsigned char depth; /* Current recursion level (to prevent stack overflows). Maxes at LW_PARSER_MAX_DEPTH */
    const unsigned char *pos; /* Current parse position */
} wkb_parse_state;

/*******************************************
Name:
ptarray_free
Purpose:
    for LWPOINT release
*******************************************/
void ptarray_free(POINTARRAY *pa)
{
    if (pa)
    {
        if (pa->serialized_pointlist && (!(((pa->flags) & 0x10) >> 4)))
            CPLFree(pa->serialized_pointlist);
        CPLFree(pa);
    }
}
/*******************************************
Name:
lwpoint_free
Purpose:
    for LWPOINT release
*******************************************/
void lwpoint_free(LWPOINT *pt)
{
    if (!pt)
        return;

    if (pt->bbox)
        CPLFree(pt->bbox);
    if (pt->point)
        ptarray_free(pt->point);
    CPLFree(pt);
}
/*******************************************
Name:
lwline_free
Purpose:
    for LWLINE release
*******************************************/
void lwline_free(LWLINE *line)
{
    if (!line)
        return;

    if (line->bbox)
        CPLFree(line->bbox);
    if (line->points)
        ptarray_free(line->points);
    CPLFree(line);
}
/*******************************************
Name:
lwpoly_free
Purpose:
    for LWPOLY release
*******************************************/
void lwpoly_free(LWPOLY *poly)
{
    unsigned int t = 0;

    if (!poly)
        return;

    if (poly->bbox)
        CPLFree(poly->bbox);

    if (poly->rings)
    {
        for (t = 0; t < poly->nrings; t++)
            if (poly->rings[t])
                ptarray_free(poly->rings[t]);
        CPLFree(poly->rings);
    }

    CPLFree(poly);
}
/*******************************************
Name:
lwcircstring_free
Purpose:
    for LWCIRCSTRING release
*******************************************/
void lwcircstring_free(LWCIRCSTRING *curve)
{
    if (!curve)
        return;

    if (curve->bbox)
        CPLFree(curve->bbox);
    if (curve->points)
        ptarray_free(curve->points);
    CPLFree(curve);
}
/*******************************************
Name:
lwtriangle_free
Purpose:
    for LWTRIANGLE release
*******************************************/
void lwtriangle_free(LWTRIANGLE *triangle)
{
    if (!triangle)
        return;

    if (triangle->bbox)
        CPLFree(triangle->bbox);

    if (triangle->points)
        ptarray_free(triangle->points);

    CPLFree(triangle);
}
/*******************************************
Name:
lwmpoint_free
Purpose:
    for LWMPOINT release
*******************************************/
void lwmpoint_free(LWMPOINT *mpt)
{
    unsigned int i = 0;

    if (!mpt)
        return;

    if (mpt->bbox)
        CPLFree(mpt->bbox);

    for (i = 0; i < mpt->ngeoms; i++)
        if (mpt->geoms && mpt->geoms[i])
            lwpoint_free(mpt->geoms[i]);

    if (mpt->geoms)
        CPLFree(mpt->geoms);

    CPLFree(mpt);
}
/*******************************************
Name:
lwmline_free
Purpose:
    for LWMLINE release
*******************************************/
void lwmline_free(LWMLINE *mline)
{
    unsigned int i = 0;
    if (!mline)
        return;

    if (mline->bbox)
        CPLFree(mline->bbox);

    if (mline->geoms)
    {
        for (i = 0; i < mline->ngeoms; i++)
            if (mline->geoms[i])
                lwline_free(mline->geoms[i]);
        CPLFree(mline->geoms);
    }

    CPLFree(mline);
}
/*******************************************
Name:
lwmpoly_free
Purpose:
    for LWMPOLY release
*******************************************/
void lwmpoly_free(LWMPOLY *mpoly)
{
    unsigned int i = 0;
    if (!mpoly)
        return;
    if (mpoly->bbox)
        CPLFree(mpoly->bbox);

    for (i = 0; i < mpoly->ngeoms; i++)
        if (mpoly->geoms && mpoly->geoms[i])
            lwpoly_free(mpoly->geoms[i]);

    if (mpoly->geoms)
        CPLFree(mpoly->geoms);

    CPLFree(mpoly);
}
/*******************************************
Name:
lwpsurface_free
Purpose:
    for LWPSURFACE release
*******************************************/
void lwpsurface_free(LWPSURFACE *psurf)
{
    unsigned int i = 0;
    if (!psurf)
        return;
    if (psurf->bbox)
        CPLFree(psurf->bbox);

    for (i = 0; i < psurf->ngeoms; i++)
        if (psurf->geoms && psurf->geoms[i])
            lwpoly_free(psurf->geoms[i]);

    if (psurf->geoms)
        CPLFree(psurf->geoms);

    CPLFree(psurf);
}
/*******************************************
Name:
lwtin_free
Purpose:
    for LWTIN release
*******************************************/
void lwtin_free(LWTIN *tin)
{
    unsigned int i = 0;
    if (!tin)
        return;
    if (tin->bbox)
        CPLFree(tin->bbox);

    for (i = 0; i < tin->ngeoms; i++)
        if (tin->geoms && tin->geoms[i])
            lwtriangle_free(tin->geoms[i]);

    if (tin->geoms)
        CPLFree(tin->geoms);

    CPLFree(tin);
}
/*******************************************
Name:
lwcollection_free
Purpose:
    for LWCOLLECTION release
*******************************************/
void lwcollection_free(LWCOLLECTION *col)
{
    unsigned int i = 0;
    if (!col)
        return;

    if (col->bbox)
    {
        CPLFree(col->bbox);
    }
    for (i = 0; i < col->ngeoms; i++)
    {
        if (col->geoms && col->geoms[i])
            lwgeom_free(col->geoms[i]);
    }
    if (col->geoms)
    {
        CPLFree(col->geoms);
    }
    CPLFree(col);
}

void lwgeom_free(LWGEOM *lwgeom)
{

    /* There's nothing here to free... */
    if (!lwgeom)
        return;

    switch (lwgeom->type)
    {
        case POINTTYPE:
            lwpoint_free((LWPOINT *)lwgeom);
            break;
        case LINETYPE:
            lwline_free((LWLINE *)lwgeom);
            break;
        case POLYGONTYPE:
            lwpoly_free((LWPOLY *)lwgeom);
            break;
        case CIRCSTRINGTYPE:
            lwcircstring_free((LWCIRCSTRING *)lwgeom);
            break;
        case TRIANGLETYPE:
            lwtriangle_free((LWTRIANGLE *)lwgeom);
            break;
        case MULTIPOINTTYPE:
            lwmpoint_free((LWMPOINT *)lwgeom);
            break;
        case MULTILINETYPE:
            lwmline_free((LWMLINE *)lwgeom);
            break;
        case MULTIPOLYGONTYPE:
            lwmpoly_free((LWMPOLY *)lwgeom);
            break;
        case POLYHEDRALSURFACETYPE:
            lwpsurface_free((LWPSURFACE *)lwgeom);
            break;
        case TINTYPE:
            lwtin_free((LWTIN *)lwgeom);
            break;
        case CURVEPOLYTYPE:
        case COMPOUNDTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case COLLECTIONTYPE:
            lwcollection_free((LWCOLLECTION *)lwgeom);
            break;
        default:
            CPLFree(lwgeom);
            return;
    }
    return;
}

/******************************************
Name:
ogr_lwflags
Purpose:
    set hasz,hasm,isGeodetic in flags
******************************************/
lwflags_t ogr_lwflags(lint hasz, lint hasm, lint geodetic)
{
    lwflags_t flags = 0;

    if (hasz)
        FLAGS_SET_Z(flags, 1);

    if (hasm)
        FLAGS_SET_M(flags, 1);

    if (geodetic)
        FLAGS_SET_GEODETIC(flags, 1);

    return flags;
}

/******************************************************
Name:
ptarray_construct_reference_data
Purpose:
    Build a new #POINTARRAY, but on top of someone else's ordinate array.
    Flag as read-only, so that ptarray_free() does not free the serialized_ptlist
*******************************************************/
POINTARRAY *ptarray_construct_reference_data(char hasz,
                                             char hasm,
                                             ulint npoints,
                                             byte *ptlist)
{
    POINTARRAY *pa = (POINTARRAY *)CPLMalloc(sizeof(POINTARRAY));
    if (!pa)
        return NULL;

    pa->flags = ogr_lwflags(hasz, hasm, 0);
    FLAGS_SET_READONLY(
        pa->flags,
        1); /* We don't own this memory, so we can't alter or free it. */
    pa->npoints = npoints;
    pa->maxpoints = npoints;
    pa->serialized_pointlist = ptlist;

    return pa;
}

/************************************************************************
Name:
ptarray_construct
Purpose:
    construct empty ptarray
**************************************************************************/
POINTARRAY *ptarray_construct_empty(char hasz,
                                    char hasm,
                                    ulint maxpoints)
{
    POINTARRAY *pa = (POINTARRAY *)CPLMalloc(sizeof(POINTARRAY));
    if (!pa)
        return NULL;

    pa->serialized_pointlist = NULL;

    /* Set our dimensionality info on the bitmap */
    pa->flags = ogr_lwflags(hasz, hasm, 0);

    /* We will be allocating a bit of room */
    pa->npoints = 0;
    pa->maxpoints = maxpoints;

    /* Allocate the coordinate array */
    if (maxpoints > 0)
    {
        /*
        * Size of point represeneted in the POINTARRAY
        * 16 for 2d, 24 for 3d, 32 for 4d
        */
        pa->serialized_pointlist = (byte *)CPLMalloc(
            maxpoints * sizeof(double) * FLAGS_NDIMS(pa->flags));
        if (!pa->serialized_pointlist)
        {
            CPLFree(pa);
            return NULL;
        }
    }
    else
        pa->serialized_pointlist = NULL;

    return pa;
}

/************************************************************
Name:
ptarray_construct
Purpose:
    construct a ptarray,and write in point.
************************************************************/
POINTARRAY *ptarray_construct(char hasz,
                              char hasm,
                              ulint npoints)
{
    POINTARRAY *pa = ptarray_construct_empty(hasz, hasm, npoints);

    pa->npoints = npoints;

    return pa;
}

/***********************************************************************
Name:
lwpoint_construct
Purpose:
    Construct a new point.  point will not be copied
    use SRID=SRID_UNKNOWN for unknown SRID (will have 8bit type's S = 0)
***********************************************************************/
LWPOINT *lwpoint_construct(lint srid,
                           GBOX *bbox,
                           POINTARRAY *point)
{
    LWPOINT *result;
    lwflags_t flags = 0;

    if (point == NULL)
        return NULL; /* error */

    result = (LWPOINT *)CPLMalloc(sizeof(LWPOINT));
    if (!result)
        return NULL;

    result->type = POINTTYPE;
    FLAGS_SET_Z(flags, FLAGS_GET_Z(point->flags));
    FLAGS_SET_M(flags, FLAGS_GET_M(point->flags));
    FLAGS_SET_BBOX(flags, bbox ? 1 : 0);
    result->flags = flags;
    result->srid = srid;
    result->point = point;
    result->bbox = bbox;

    return result;
}

/***********************************************************************
Name:
lwpoint_from_gserialized_buffer
Purpose:
    write gserialized into point lwgeom.
***********************************************************************/
static LWPOINT *lwpoint_from_gserialized_buffer(byte *data_ptr,
                                                lwflags_t lwflags,
                                                lint srid)
{
    LWPOINT *point;
    ulint npoints = 0;

    point = (LWPOINT *)CPLMalloc(sizeof(LWPOINT));
    if (!point)
        return NULL;

    point->srid = srid;
    point->bbox = NULL;
    point->type = POINTTYPE;
    point->flags = lwflags;

    data_ptr += 4;                    /* Skip past the type. */
    npoints = *((ulint *)(data_ptr)); /* Zero => empty geometry */
    data_ptr += 4;                    /* Skip past the npoints. */

    if (npoints > 0)
        point->point = ptarray_construct_reference_data(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), 1, data_ptr);
    else
        point->point = ptarray_construct(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), 0); /* Empty point */
    if (!point->point)
    {
        lwpoint_free(point);
        return NULL;
    }
    data_ptr += npoints * FLAGS_NDIMS(lwflags) * sizeof(double);

    return point;
}

/***********************************************************************
Name:
lwline_from_gserialized_buffer
Purpose:
    write gserialized into linestring lwgeom
***********************************************************************/
static LWLINE *lwline_from_gserialized_buffer(byte *data_ptr,
                                              lwflags_t lwflags,
                                              lint srid)
{
    LWLINE *line;
    ulint npoints = 0;

    line = (LWLINE *)CPLMalloc(sizeof(LWLINE));
    if (!line)
        return NULL;

    line->srid = srid;
    line->bbox = NULL;
    line->type = LINETYPE;
    line->flags = lwflags;

    data_ptr += 4;                    /* Skip past the type. */
    npoints = *((ulint *)(data_ptr)); /* Zero => empty geometry */
    data_ptr += 4;                    /* Skip past the npoints. */

    if (npoints > 0)
        line->points = ptarray_construct_reference_data(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), npoints, data_ptr);
    else
        line->points =
            ptarray_construct(FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags),
                              0); /* Empty linestring */
    if (!line->points)
    {
        CPLFree(line);
        return NULL;
    }
    data_ptr += FLAGS_NDIMS(lwflags) * npoints * sizeof(double);

    return line;
}

/***********************************************************************
Name:
lwcircstring_from_gserialized_buffer
Purpose:
    write gserialized into circstring lwgeom
***********************************************************************/
static LWCIRCSTRING *lwcircstring_from_gserialized_buffer(byte *data_ptr,
                                                          lwflags_t lwflags,
                                                          lint srid)
{
    LWCIRCSTRING *circstring;
    ulint npoints = 0;

    circstring = (LWCIRCSTRING *)CPLMalloc(sizeof(LWCIRCSTRING));
    if (!circstring)
        return NULL;

    circstring->srid = srid;
    circstring->bbox = NULL;
    circstring->type = CIRCSTRINGTYPE;
    circstring->flags = lwflags;

    data_ptr += 4;                    /* Skip past the circstringtype. */
    npoints = *((ulint *)(data_ptr)); /* Zero => empty geometry */
    data_ptr += 4;                    /* Skip past the npoints. */

    if (npoints > 0)
        circstring->points = ptarray_construct_reference_data(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), npoints, data_ptr);
    else
        circstring->points =
            ptarray_construct(FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags),
                              0); /* Empty circularstring */
    if (!circstring->points)
    {
        CPLFree(circstring);
        return NULL;
    }

    data_ptr += FLAGS_NDIMS(lwflags) * npoints * sizeof(double);

    return circstring;
}

/***********************************************************************
Name:
lwpoly_from_gserialized_buffer
Purpose:
    write gserialized into polygon lwgeom
***********************************************************************/
static LWPOLY *lwpoly_from_gserialized_buffer(byte *data_ptr,
                                              lwflags_t lwflags,
                                              lint srid)
{
    LWPOLY *poly;
    byte *ordinate_ptr;
    ulint nrings = 0;
    ulint i = 0;
    ulint j = 0;

    poly = (LWPOLY *)CPLMalloc(sizeof(LWPOLY));
    if (!poly)
        return NULL;

    poly->srid = srid;
    poly->bbox = NULL;
    poly->type = POLYGONTYPE;
    poly->flags = lwflags;

    data_ptr += 4;                   /* Skip past the polygontype. */
    nrings = *((ulint *)(data_ptr)); /* Zero => empty geometry */
    poly->nrings = nrings;
    data_ptr += 4; /* Skip past the nrings. */

    ordinate_ptr = data_ptr; /* Start the ordinate pointer. */
    if (nrings > 0)
    {
        poly->rings = (POINTARRAY **)CPLMalloc(sizeof(POINTARRAY *) * nrings);
        if (!poly->rings)
        {
            CPLFree(poly);
            return NULL;
        }

        poly->maxrings = nrings;
        ordinate_ptr += nrings * 4; /* Move past all the npoints values. */
        if (nrings % 2) /* If there is padding, move past that too. */
            ordinate_ptr += 4;
    }
    else /* Empty polygon */
    {
        poly->rings = NULL;
        poly->maxrings = 0;
    }

    for (i = 0; i < nrings; i++)
    {
        ulint npoints = 0;

        /* Read in the number of points. */
        npoints = *((ulint *)(data_ptr));
        data_ptr += 4;

        /* Make a point array for the ring, and move the ordinate pointer past the ring ordinates. */
        poly->rings[i] = ptarray_construct_reference_data(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), npoints, ordinate_ptr);
        if (!poly->rings[i])
        {
            for (j = 0; j < i; j++)
            {
                CPLFree(poly->rings[j]);
            }
            CPLFree(poly);
            return NULL;
        }
        ordinate_ptr += sizeof(double) * FLAGS_NDIMS(lwflags) * npoints;
    }

    return poly;
}

/***********************************************************************
Name:
lwtriangle_from_gserialized_buffer
Purpose:
    write gserialized into triangle lwgeom
***********************************************************************/
static LWTRIANGLE *
lwtriangle_from_gserialized_buffer(byte *data_ptr,
                                   lwflags_t lwflags,
                                   lint srid)
{
    LWTRIANGLE *triangle;
    ulint npoints = 0;

    triangle = (LWTRIANGLE *)CPLMalloc(sizeof(LWTRIANGLE));
    if (!triangle)
        return NULL;

    triangle->srid = srid; /* Default */
    triangle->bbox = NULL;
    triangle->type = TRIANGLETYPE;
    triangle->flags = lwflags;

    data_ptr += 4;                    /* Skip past the type. */
    npoints = *((ulint *)(data_ptr)); /* Zero => empty geometry */
    data_ptr += 4;                    /* Skip past the npoints. */

    if (npoints > 0)
        triangle->points = ptarray_construct_reference_data(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), npoints, data_ptr);
    else
        triangle->points = ptarray_construct(
            FLAGS_GET_Z(lwflags), FLAGS_GET_M(lwflags), 0); /* Empty triangle */
    if (!triangle->points)
    {
        CPLFree(triangle);
        return NULL;
    }
    data_ptr += FLAGS_NDIMS(lwflags) * npoints * sizeof(double);

    return triangle;
}

/***********************************************************************
Name:
lwcollection_allows_subtype
Purpose:
    Determine whether the collection type is legal.
***********************************************************************/
int lwcollection_allows_subtype(int collectiontype,
                                int subtype)
{
    if (collectiontype == COLLECTIONTYPE)
        return LW_TRUE;
    if (collectiontype == MULTIPOINTTYPE && subtype == POINTTYPE)
        return LW_TRUE;
    if (collectiontype == MULTILINETYPE && subtype == LINETYPE)
        return LW_TRUE;
    if (collectiontype == MULTIPOLYGONTYPE && subtype == POLYGONTYPE)
        return LW_TRUE;
    if (collectiontype == COMPOUNDTYPE &&
        (subtype == LINETYPE || subtype == CIRCSTRINGTYPE))
        return LW_TRUE;
    if (collectiontype == CURVEPOLYTYPE &&
        (subtype == CIRCSTRINGTYPE || subtype == LINETYPE ||
         subtype == COMPOUNDTYPE))
        return LW_TRUE;
    if (collectiontype == MULTICURVETYPE &&
        (subtype == CIRCSTRINGTYPE || subtype == LINETYPE ||
         subtype == COMPOUNDTYPE))
        return LW_TRUE;
    if (collectiontype == MULTISURFACETYPE &&
        (subtype == POLYGONTYPE || subtype == CURVEPOLYTYPE))
        return LW_TRUE;
    if (collectiontype == POLYHEDRALSURFACETYPE && subtype == POLYGONTYPE)
        return LW_TRUE;
    if (collectiontype == TINTYPE && subtype == TRIANGLETYPE)
        return LW_TRUE;

    /* Must be a bad combination! */
    return LW_FALSE;
}

/***********************************************************************
Name:
lwcollection_from_gserialized_buffer
Purpose:
    write gserialized into collection lwgeom.
***********************************************************************/
static LWCOLLECTION *lwcollection_from_gserialized_buffer(byte *data_ptr,
                                                          lwflags_t lwflags,
                                                          lint srid)
{
    ulint type;
    LWCOLLECTION *collection;
    ulint ngeoms = 0;
    ulint i = 0;
    ulint j = 0;

    type = *((ulint *)(data_ptr));
    data_ptr += 4; /* Skip past the type. */

    collection = (LWCOLLECTION *)CPLMalloc(sizeof(LWCOLLECTION));
    if (!collection)
        return NULL;

    collection->srid = srid;
    collection->bbox = NULL;
    collection->type = (byte)type;
    collection->flags = lwflags;

    ngeoms = *((ulint *)(data_ptr));
    collection->ngeoms = ngeoms; /* Zero => empty geometry */
    data_ptr += 4;               /* Skip past the ngeoms. */

    if (ngeoms > 0)
    {
        collection->geoms = (LWGEOM **)CPLMalloc(sizeof(LWGEOM *) * ngeoms);
        if (!collection->geoms)
        {
            CPLFree(collection);
            return NULL;
        }

        collection->maxgeoms = ngeoms;
    }
    else
    {
        collection->geoms = NULL;
        collection->maxgeoms = 0;
    }

    /* Sub-geometries are never de-serialized with boxes (#1254) */
    FLAGS_SET_BBOX(lwflags, 0);

    for (i = 0; i < ngeoms; i++)
    {
        ulint subtype = *((ulint *)(data_ptr));
        ulint64 subsize = 0;

        if (!lwcollection_allows_subtype(type, subtype))
        {
            for (j = 0; j < i; j++)
            {
                CPLFree(collection->geoms[j]);
            }
            CPLFree(collection);
            return NULL;
        }
        collection->geoms[i] =
            lwgeom_from_gserialized_buffer(data_ptr, lwflags, srid);
        if (!collection->geoms[i])
        {
            for (j = 0; j < i; j++)
            {
                CPLFree(collection->geoms[j]);
            }
            CPLFree(collection);
            return NULL;
        }
        data_ptr += subsize;
    }

    return collection;
}

/***********************************************************************
Name:
lwgeom_from_gserialized_buffer
Purpose:
    write gserialized into lwgeom.
***********************************************************************/
LWGEOM *lwgeom_from_gserialized_buffer(byte *data_ptr,
                                       lwflags_t lwflags,
                                       lint srid)
{
    ulint type;

    type = *((ulint *)(data_ptr));

    switch (type)
    {
        case POINTTYPE:
            return (LWGEOM *)lwpoint_from_gserialized_buffer(data_ptr, lwflags,
                                                             srid);
        case LINETYPE:
            return (LWGEOM *)lwline_from_gserialized_buffer(data_ptr, lwflags,
                                                            srid);
        case CIRCSTRINGTYPE:
            return (LWGEOM *)lwcircstring_from_gserialized_buffer(
                data_ptr, lwflags, srid);
        case POLYGONTYPE:
            return (LWGEOM *)lwpoly_from_gserialized_buffer(data_ptr, lwflags,
                                                            srid);
        case TRIANGLETYPE:
            return (LWGEOM *)lwtriangle_from_gserialized_buffer(data_ptr,
                                                                lwflags, srid);
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return (LWGEOM *)lwcollection_from_gserialized_buffer(
                data_ptr, lwflags, srid);
        default:
            return NULL;
    }
}

/*******************************************************
Name:
lwpoint_is_empty
Purpose:
    determine whether lwpoint is empty
*******************************************************/
static int lwpoint_is_empty(const LWPOINT *point)
{
    return !point->point || point->point->npoints < 1;
}

/*******************************************************
Name:
lwline_is_empty
Purpose:
    determine whether lwline is empty
*******************************************************/
static int lwline_is_empty(const LWLINE *line)
{
    return !line->points || line->points->npoints < 1;
}

/*******************************************************
Name:
lwcircstring_is_empty
Purpose:
    determine whether lwcircstring is empty
*******************************************************/
static int lwcircstring_is_empty(const LWCIRCSTRING *circ)
{
    return !circ->points || circ->points->npoints < 1;
}

/*******************************************************
Name:
lwpoly_is_empty
Purpose:
    determine whether lwpoly is empty
*******************************************************/
static int lwpoly_is_empty(const LWPOLY *poly)
{
    return poly->nrings < 1 || !poly->rings || !poly->rings[0] ||
           poly->rings[0]->npoints < 1;
}

/*******************************************************
Name:
lwtriangle_is_empty
Purpose:
    determine whether lwtriangle is empty
*******************************************************/
static int lwtriangle_is_empty(const LWTRIANGLE *triangle)
{
    return !triangle->points || triangle->points->npoints < 1;
}

/*******************************************************
Name:
lwcollection_is_empty
Purpose:
    determine whether lwcollection is empty
*******************************************************/
static int lwcollection_is_empty(const LWCOLLECTION *col)
{
    ulint i;

    if (col->ngeoms == 0 || !col->geoms)
        return LW_TRUE;

    for (i = 0; i < col->ngeoms; i++)
    {
        if (!lwgeom_is_empty(col->geoms[i]))
            return LW_FALSE;
    }

    return LW_TRUE;
}

/*******************************************************
Name:
lwgeom_is_empty
Purpose:
    determine whether lwgeom is empty
*******************************************************/
static int lwgeom_is_empty(const LWGEOM *geom)
{
    switch (geom->type)
    {
        case POINTTYPE:
            return lwpoint_is_empty((LWPOINT *)geom);
            break;
        case LINETYPE:
            return lwline_is_empty((LWLINE *)geom);
            break;
        case CIRCSTRINGTYPE:
            return lwcircstring_is_empty((LWCIRCSTRING *)geom);
            break;
        case POLYGONTYPE:
            return lwpoly_is_empty((LWPOLY *)geom);
            break;
        case TRIANGLETYPE:
            return lwtriangle_is_empty((LWTRIANGLE *)geom);
            break;
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return lwcollection_is_empty((LWCOLLECTION *)geom);
            break;
        default:
            return LW_FALSE;
            break;
    }
}

/*******************************************************
Name:
lwgeom_wkb_needs_srid
Purpose:
    determine if srid is required
*******************************************************/
static int lwgeom_wkb_needs_srid(const LWGEOM *geom,
                                 byte variant)
{
    if (variant & WKB_NO_SRID)
        return LW_FALSE;

    if ((variant & WKB_EXTENDED) && geom->srid != SRID_UNKNOWN)
        return LW_FALSE;

    return LW_FALSE;
}

/*******************************************************
Name:
empty_to_wkb_size
Purpose:
    get wkb size
*******************************************************/
static ulint64 empty_to_wkb_size(const LWGEOM *geom,
                                 byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE;

    if (lwgeom_wkb_needs_srid(geom, variant))
        size += WKB_INT_SIZE;

    if (geom->type == POINTTYPE)
    {
        const LWPOINT *pt = (LWPOINT *)geom;
        size += WKB_DOUBLE_SIZE * FLAGS_NDIMS(pt->point->flags);
    }
    else
    {
        size += WKB_INT_SIZE;
    }

    return size;
}

/*******************************************************
Name:
lwgeom_wkb_type
Purpose:
    get wkb type
*******************************************************/
static ulint lwgeom_wkb_type(const LWGEOM *geom,
                             byte variant)
{
    ulint wkb_type = 0;

    switch (geom->type)
    {
        case POINTTYPE:
            wkb_type = WKB_POINT_TYPE;
            break;
        case LINETYPE:
            wkb_type = WKB_LINESTRING_TYPE;
            break;
        case POLYGONTYPE:
            wkb_type = WKB_POLYGON_TYPE;
            break;
        case MULTIPOINTTYPE:
            wkb_type = WKB_MULTIPOINT_TYPE;
            break;
        case MULTILINETYPE:
            wkb_type = WKB_MULTILINESTRING_TYPE;
            break;
        case MULTIPOLYGONTYPE:
            wkb_type = WKB_MULTIPOLYGON_TYPE;
            break;
        case COLLECTIONTYPE:
            wkb_type = WKB_GEOMETRYCOLLECTION_TYPE;
            break;
        case CIRCSTRINGTYPE:
            wkb_type = WKB_CIRCULARSTRING_TYPE;
            break;
        case COMPOUNDTYPE:
            wkb_type = WKB_COMPOUNDCURVE_TYPE;
            break;
        case CURVEPOLYTYPE:
            wkb_type = WKB_CURVEPOLYGON_TYPE;
            break;
        case MULTICURVETYPE:
            wkb_type = WKB_MULTICURVE_TYPE;
            break;
        case MULTISURFACETYPE:
            wkb_type = WKB_MULTISURFACE_TYPE;
            break;
        case POLYHEDRALSURFACETYPE:
            wkb_type = WKB_POLYHEDRALSURFACE_TYPE;
            break;
        case TINTYPE:
            wkb_type = WKB_TIN_TYPE;
            break;
        case TRIANGLETYPE:
            wkb_type = WKB_TRIANGLE_TYPE;
            break;
        default:
            break;
    }

    if (variant & WKB_EXTENDED)
    {
        if (FLAGS_GET_Z(geom->flags))
            wkb_type |= WKBZOFFSET;

        if (FLAGS_GET_M(geom->flags))
            wkb_type |= WKBMOFFSET;
        /*		if ( geom->srid != SRID_UNKNOWN && ! (variant & WKB_NO_SRID) ) */
        if (lwgeom_wkb_needs_srid(geom, variant))
            wkb_type |= WKBSRIDFLAG;
    }
    else if (variant & WKB_ISO)
    {
        /* Z types are in the 1000 range */
        if (FLAGS_GET_Z(geom->flags))
            wkb_type += 1000;
        /* M types are in the 2000 range */
        if (FLAGS_GET_M(geom->flags))
            wkb_type += 2000;
        /* ZM types are in the 1000 + 2000 = 3000 range, see above */
    }

    return wkb_type;
}

/***********************************************
Name:
endian_to_wkb_buf
Purpose:
    Endian
************************************************/
static byte *endian_to_wkb_buf(byte *buf,
                               byte variant)
{
    if (variant & WKB_HEX)
    {
        buf[0] = '0';
        buf[1] = ((variant & WKB_NDR) ? '1' : '0');
        return buf + 2;
    }
    else
    {
        buf[0] = ((variant & WKB_NDR) ? 1 : 0);
        return buf + 1;
    }
}

/**********************************************
Name:
wkb_swap_bytes
Purpose:
    SwapBytes
***********************************************/
static int wkb_swap_bytes(byte variant)
{
    if (((variant & WKB_NDR) && !IS_BIG_ENDIAN) ||
        ((!(variant & WKB_NDR)) && IS_BIG_ENDIAN))
    {
        return LW_FALSE;
    }

    return LW_TRUE;
}

static char *hexchr = "0123456789ABCDEF";

/**********************************************
Name:
integer_to_wkb_buf
Purpose:
    integer to wkb
***********************************************/
static byte *integer_to_wkb_buf(const ulint ival,
                                byte *buf,
                                byte variant)
{
    byte *iptr = (byte *)(&ival);
    int i = 0;

    if (variant & WKB_HEX)
    {
        int swap = wkb_swap_bytes(variant);
        for (i = 0; i < WKB_INT_SIZE; i++)
        {
            int j = (swap ? WKB_INT_SIZE - 1 - i : i);
            byte b = iptr[j];
            buf[2 * i] = hexchr[b >> 4];
            buf[2 * i + 1] = hexchr[b & 0x0F];
        }

        return buf + (2 * WKB_INT_SIZE);
    }
    else
    {
        if (wkb_swap_bytes(variant))
        {
            for (i = 0; i < WKB_INT_SIZE; i++)
            {
                buf[i] = iptr[WKB_INT_SIZE - 1 - i];
            }
        }
        else
        {
            memcpy(buf, iptr, WKB_INT_SIZE);
        }

        return buf + WKB_INT_SIZE;
    }
}

/**********************************************
Name:
double_nan_to_wkb_buf
Purpose:
    double to wkb
***********************************************/
static byte *double_nan_to_wkb_buf(byte *buf,
                                   byte variant)
{
#define NAN_SIZE 8
    const byte ndr_nan[NAN_SIZE] = {0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0xf8, 0x7f};
    const byte xdr_nan[NAN_SIZE] = {0x7f, 0xf8, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00};
    int i;

    if (variant & WKB_HEX)
    {
        for (i = 0; i < NAN_SIZE; i++)
        {
            byte b = (variant & WKB_NDR) ? ndr_nan[i] : xdr_nan[i];
            /* Top four bits to 0-F */
            buf[2 * i] = hexchr[b >> 4];
            /* Bottom four bits to 0-F */
            buf[2 * i + 1] = hexchr[b & 0x0F];
        }

        return buf + (2 * NAN_SIZE);
    }
    else
    {
        for (i = 0; i < NAN_SIZE; i++)
        {
            buf[i] = (variant & WKB_NDR) ? ndr_nan[i] : xdr_nan[i];
            ;
        }

        return buf + NAN_SIZE;
    }
}

/**********************************************
Name:
empty_to_wkb_buf
Purpose:
    empty to wkb
***********************************************/
static byte *empty_to_wkb_buf(const LWGEOM *geom,
                              byte *buf,
                              byte variant)
{
    ulint wkb_type = lwgeom_wkb_type(geom, variant);
    int i;

    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(wkb_type, buf, variant);
    if (lwgeom_wkb_needs_srid(geom, variant))
        buf = integer_to_wkb_buf(geom->srid, buf, variant);

    if (geom->type == POINTTYPE)
    {
        const LWPOINT *pt = (LWPOINT *)geom;
        for (i = 0; i < FLAGS_NDIMS(pt->point->flags); i++)
            buf = double_nan_to_wkb_buf(buf, variant);
    }
    else
    {
        buf = integer_to_wkb_buf(0, buf, variant);
    }

    return buf;
}

/**********************************************
Name:
ptarray_to_wkb_size
Purpose:
    get wkb size
***********************************************/
static ulint64 ptarray_to_wkb_size(const POINTARRAY *pa,
                                   byte variant)
{
    int dims = 2;
    ulint64 size = 0;

    if (variant & (WKB_ISO | WKB_EXTENDED))
        dims = FLAGS_NDIMS(pa->flags);

    if (!(variant & WKB_NO_NPOINTS))
        size += WKB_INT_SIZE;

    size += pa->npoints * dims * WKB_DOUBLE_SIZE;

    return size;
}

/**********************************************
Name:
get_point_internal
Purpose:
    get point internal
***********************************************/
static byte *get_point_internal(const POINTARRAY *pa,
                                ulint n)
{
    ulint64 size;
    byte *ptr;

    size = sizeof(double) * FLAGS_NDIMS(pa->flags);
    ptr = pa->serialized_pointlist + size * n;

    return ptr;
}

/******************************************
Name:
double_to_wkb_buf
Purpose:
    Float64
******************************************/
static byte *double_to_wkb_buf(const double d,
                               byte *buf,
                               byte variant)
{
    byte *dptr = (byte *)(&d);
    int i = 0;
    int swap;
    int j;
    byte b;

    if (variant & WKB_HEX)
    {
        swap = wkb_swap_bytes(variant);
        /* Machine/request arch mismatch, so flip byte order */
        for (i = 0; i < WKB_DOUBLE_SIZE; i++)
        {
            j = (swap ? WKB_DOUBLE_SIZE - 1 - i : i);
            b = dptr[j];
            /* Top four bits to 0-F */
            buf[2 * i] = hexchr[b >> 4];
            /* Bottom four bits to 0-F */
            buf[2 * i + 1] = hexchr[b & 0x0F];
        }

        return buf + (2 * WKB_DOUBLE_SIZE);
    }
    else
    {
        /* Machine/request arch mismatch, so flip byte order */
        if (wkb_swap_bytes(variant))
        {
            for (i = 0; i < WKB_DOUBLE_SIZE; i++)
            {
                buf[i] = dptr[WKB_DOUBLE_SIZE - 1 - i];
            }
        }
        /* If machine arch and requested arch match, don't flip byte order */
        else
        {
            memcpy(buf, dptr, WKB_DOUBLE_SIZE);
        }

        return buf + WKB_DOUBLE_SIZE;
    }
}

/******************************************
Name:
ptarray_to_wkb_buf
Purpose:
    write ptarray
******************************************/
static byte *ptarray_to_wkb_buf(const POINTARRAY *pa,
                                byte *buf,
                                byte variant)
{
    ulint dims = 2;
    ulint pa_dims = FLAGS_NDIMS(pa->flags);
    ulint i, j;
    double *dbl_ptr;

    if ((variant & WKB_ISO) || (variant & WKB_EXTENDED))
        dims = pa_dims;

    if (!(variant & WKB_NO_NPOINTS))
        buf = integer_to_wkb_buf(pa->npoints, buf, variant);

    if (pa->npoints && (dims == pa_dims) && !wkb_swap_bytes(variant) &&
        !(variant & WKB_HEX))
    {
        ulint64 size = pa->npoints * dims * WKB_DOUBLE_SIZE;
        memcpy(buf, get_point_internal(pa, 0), size);
        buf += size;
    }
    else
    {
        for (i = 0; i < pa->npoints; i++)
        {
            dbl_ptr = (double *)get_point_internal(pa, i);
            for (j = 0; j < dims; j++)
            {
                buf = double_to_wkb_buf(dbl_ptr[j], buf, variant);
            }
        }
    }

    return buf;
}

/*******************************************************
Name:
lwpoint_to_wkb_size
Purpose:
    POINT
*******************************************************/
static ulint64 lwpoint_to_wkb_size(const LWPOINT *pt,
                                   byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE;
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)pt))
        return empty_to_wkb_size((LWGEOM *)pt, variant);

    if (lwgeom_wkb_needs_srid((LWGEOM *)pt, variant))
        size += WKB_INT_SIZE;

    size += ptarray_to_wkb_size(pt->point, variant | WKB_NO_NPOINTS);
    return size;
}

/*******************************************************
Name:
lwpoint_to_wkb_buf
Purpose:
    POINT
*******************************************************/
static byte *lwpoint_to_wkb_buf(const LWPOINT *pt,
                                byte *buf,
                                byte variant)
{
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)pt))
        return empty_to_wkb_buf((LWGEOM *)pt, buf, variant);
    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)pt, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)pt, variant))
        buf = integer_to_wkb_buf(pt->srid, buf, variant);

    buf = ptarray_to_wkb_buf(pt->point, buf, variant | WKB_NO_NPOINTS);
    return buf;
}

/*******************************************************
Name:
lwline_to_wkb_size
Purpose:
    LINESTRING
*******************************************************/
static ulint64 lwline_to_wkb_size(const LWLINE *line,
                                  byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE;
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)line))
        return empty_to_wkb_size((LWGEOM *)line, variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)line, variant))
        size += WKB_INT_SIZE;

    size += ptarray_to_wkb_size(line->points, variant);
    return size;
}

/*******************************************************
Name:
lwline_to_wkb_buf
Purpose:
    LINESTRING
*******************************************************/
static byte *lwline_to_wkb_buf(const LWLINE *line,
                               byte *buf,
                               byte variant)
{
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)line))
        return empty_to_wkb_buf((LWGEOM *)line, buf, variant);
    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)line, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)line, variant))
        buf = integer_to_wkb_buf(line->srid, buf, variant);

    buf = ptarray_to_wkb_buf(line->points, buf, variant);
    return buf;
}

/*******************************************************
Name:
lwtriangle_to_wkb_size
Purpose:
    TRIANGLE
*******************************************************/
static ulint64 lwtriangle_to_wkb_size(const LWTRIANGLE *tri,
                                      byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE + WKB_INT_SIZE;
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)tri))
        return empty_to_wkb_size((LWGEOM *)tri, variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)tri, variant))
        size += WKB_INT_SIZE;

    size += ptarray_to_wkb_size(tri->points, variant);

    return size;
}

/*******************************************************
Name:
lwtriangle_to_wkb_buf
Purpose:
    TRIANGLE
*******************************************************/
static byte *lwtriangle_to_wkb_buf(const LWTRIANGLE *tri,
                                   byte *buf,
                                   byte variant)
{
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)tri))
        return empty_to_wkb_buf((LWGEOM *)tri, buf, variant);
    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)tri, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)tri, variant))
        buf = integer_to_wkb_buf(tri->srid, buf, variant);

    buf = integer_to_wkb_buf(1, buf, variant);
    buf = ptarray_to_wkb_buf(tri->points, buf, variant);

    return buf;
}

/*******************************************************
Name:
lwpoly_to_wkb_size
Purpose:
    POLYGON
*******************************************************/
static ulint64 lwpoly_to_wkb_size(const LWPOLY *poly,
                                  byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE + WKB_INT_SIZE;
    ulint i = 0;
    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)poly))
        return empty_to_wkb_size((LWGEOM *)poly, variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)poly, variant))
        size += WKB_INT_SIZE;

    for (i = 0; i < poly->nrings; i++)
    {
        size += ptarray_to_wkb_size(poly->rings[i], variant);
    }

    return size;
}

/*******************************************************
Name:
lwpoly_to_wkb_buf
Purpose:
    POLYGON
*******************************************************/
static byte *lwpoly_to_wkb_buf(const LWPOLY *poly,
                               byte *buf,
                               byte variant)
{
    ulint i;

    if ((variant & WKB_EXTENDED) && lwgeom_is_empty((LWGEOM *)poly))
        return empty_to_wkb_buf((LWGEOM *)poly, buf, variant);
    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)poly, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)poly, variant))
        buf = integer_to_wkb_buf(poly->srid, buf, variant);

    buf = integer_to_wkb_buf(poly->nrings, buf, variant);

    for (i = 0; i < poly->nrings; i++)
    {
        buf = ptarray_to_wkb_buf(poly->rings[i], buf, variant);
    }

    return buf;
}

/*******************************************************
Name:
lwcollection_to_wkb_size
Purpose:
    COLLECTION
*******************************************************/
static ulint64 lwcollection_to_wkb_size(const LWCOLLECTION *col,
                                        byte variant)
{
    ulint64 size = WKB_BYTE_SIZE + WKB_INT_SIZE + WKB_INT_SIZE;
    ulint i = 0;

    if (lwgeom_wkb_needs_srid((LWGEOM *)col, variant))
        size += WKB_INT_SIZE;

    for (i = 0; i < col->ngeoms; i++)
    {
        size +=
            lwgeom_to_wkb_size((LWGEOM *)col->geoms[i], variant | WKB_NO_SRID);
    }

    return size;
}

/*******************************************************
Name:
lwcompound_to_wkb_buf
Purpose:
    COMPOUND
*******************************************************/
static byte *lwcompound_to_wkb_buf(const LWCOMPOUND *com,
                                   byte *buf,
                                   byte variant)
{
    ulint i;

    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)com, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)com, variant))
        buf = integer_to_wkb_buf(com->srid, buf, variant);
    buf = integer_to_wkb_buf(com->ngeoms, buf, variant);

    for (i = 0; i < com->ngeoms; i++)
    {
        buf = lwgeom_to_wkb_buf(com->geoms[i], buf, variant | WKB_NO_SRID);
    }

    return buf;
}

/*******************************************************
Name:
lwcollection_to_wkb_buf
Purpose:
    COLLECTION
*******************************************************/
static byte *lwcollection_to_wkb_buf(const LWCOLLECTION *col,
                                     byte *buf,
                                     byte variant)
{
    ulint i;

    buf = endian_to_wkb_buf(buf, variant);
    buf = integer_to_wkb_buf(lwgeom_wkb_type((LWGEOM *)col, variant), buf,
                             variant);
    if (lwgeom_wkb_needs_srid((LWGEOM *)col, variant))
        buf = integer_to_wkb_buf(col->srid, buf, variant);
    buf = integer_to_wkb_buf(col->ngeoms, buf, variant);

    for (i = 0; i < col->ngeoms; i++)
    {
        buf = lwgeom_to_wkb_buf(col->geoms[i], buf, variant | WKB_NO_SRID);
    }

    return buf;
}

/*******************************************************
Name:
lwgeom_to_wkb_size
Purpose:
    LWGEOM
*******************************************************/
static ulint64 lwgeom_to_wkb_size(const LWGEOM *geom,
                                  byte variant)
{
    ulint64 size = 0;

    if (geom == NULL)
    {
        return 0;
    }

    if ((!(variant & WKB_EXTENDED)) && lwgeom_is_empty(geom))
    {
        return empty_to_wkb_size(geom, variant);
    }

    switch (geom->type)
    {
        case POINTTYPE:
            size += lwpoint_to_wkb_size((LWPOINT *)geom, variant);
            break;
        case CIRCSTRINGTYPE:
        case LINETYPE:
            size += lwline_to_wkb_size((LWLINE *)geom, variant);
            break;
            /* Polygon has nrings and rings elements */
        case POLYGONTYPE:
            size += lwpoly_to_wkb_size((LWPOLY *)geom, variant);
            break;
            /* Triangle has one ring of three points */
        case TRIANGLETYPE:
            size += lwtriangle_to_wkb_size((LWTRIANGLE *)geom, variant);
            break;
            /* All these Collection types have ngeoms and geoms elements */
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case COLLECTIONTYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
            size += lwcollection_to_wkb_size((LWCOLLECTION *)geom, variant);
            break;
            /* Unknown type! */
        default:
            return 0;
    }
    return size;
}

/*******************************************************
Name:
lwgeom_to_wkb_buf
Purpose:
    LWGEOM
*******************************************************/
static byte *lwgeom_to_wkb_buf(const LWGEOM *geom,
                               byte *buf,
                               byte variant)
{
    if (lwgeom_is_empty(geom) && !(variant & WKB_EXTENDED))
        return empty_to_wkb_buf(geom, buf, variant);

    switch (geom->type)
    {
        case POINTTYPE:
            return lwpoint_to_wkb_buf((LWPOINT *)geom, buf, variant);
        case CIRCSTRINGTYPE:
        case LINETYPE:
            return lwline_to_wkb_buf((LWLINE *)geom, buf, variant);
            /* Polygon has 'nrings' and 'rings' elements */
        case POLYGONTYPE:
            return lwpoly_to_wkb_buf((LWPOLY *)geom, buf, variant);
        case TRIANGLETYPE:
            return lwtriangle_to_wkb_buf((LWTRIANGLE *)geom, buf, variant);
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
            return lwcompound_to_wkb_buf((LWCOMPOUND *)geom, buf, variant);
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case COLLECTIONTYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
            return lwcollection_to_wkb_buf((LWCOLLECTION *)geom, buf, variant);
            /* Unknown type! */
        default:
            return 0;
    }
}

/*******************************************************
Name:
lwgeom_to_wkb_write_buf
Purpose:
    LWGEOM
*******************************************************/
static lint64 lwgeom_to_wkb_write_buf(const LWGEOM *geom,
                                      byte variant,
                                      byte *buffer)
{
    lint64 written_bytes;

    if (!(variant & WKB_NDR || variant & WKB_XDR) ||
        (variant & WKB_NDR && variant & WKB_XDR))
    {
        if (IS_BIG_ENDIAN)
            variant = variant | WKB_XDR;
        else
            variant = variant | WKB_NDR;
    }

    written_bytes = (lint64)(lwgeom_to_wkb_buf(geom, buffer, variant) - buffer);
    return written_bytes;
}

/*******************************************************
Name:
lwgeom_to_wkb_buffer
Purpose:
    LWGEOM
*******************************************************/
byte *lwgeom_to_wkb_buffer(const LWGEOM *geom,
                           byte variant,
                           ulint64 *b_size)
{
    byte *buffer;
    lint64 written_size;

    *b_size = lwgeom_to_wkb_size(geom, variant);
    if (variant & WKB_HEX)
    {
        *b_size = 2 * *b_size + 1;
    }

    buffer = (byte *)CPLMalloc(*b_size);
    if (!buffer)
        return NULL;

    written_size = lwgeom_to_wkb_write_buf(geom, variant, buffer);
    if (variant & WKB_HEX)
    {
        buffer[written_size] = '\0';
        written_size++;
    }

    return buffer;
}

LWGEOM *lwgeom_from_gserialized(GSERIALIZED *geom)
{
    ulint64 xflags = 0;
    lint srid = 0;
    lwflags_t lwflags = 0;
    ulint extra_data_bytes = 0;
    byte *data_ptr = NULL;

    srid = srid | (geom->srid[0] << 16);
    srid = srid | (geom->srid[1] << 8);
    srid = srid | (geom->srid[2]);
    srid = (srid << 11) >> 11;
    if (srid == 0)
        srid = SRID_UNKNOWN;

    if (G2FLAGS_GET_EXTENDED(geom->gflags))
        extra_data_bytes += sizeof(ulint64);

    if (G2FLAGS_GET_BBOX(geom->gflags))
    {
        if (G2FLAGS_GET_GEODETIC(geom->gflags))
            extra_data_bytes += 6 * sizeof(float);
        else
            extra_data_bytes += 2 * G2FLAGS_NDIMS(geom->gflags) * sizeof(float);
    }

    FLAGS_SET_Z(lwflags, G2FLAGS_GET_Z(geom->gflags));
    FLAGS_SET_M(lwflags, G2FLAGS_GET_M(geom->gflags));
    FLAGS_SET_BBOX(lwflags, G2FLAGS_GET_BBOX(geom->gflags));
    FLAGS_SET_GEODETIC(lwflags, G2FLAGS_GET_GEODETIC(geom->gflags));
    if (G2FLAGS_GET_EXTENDED(geom->gflags))
    {
        memcpy(&xflags, geom->data, sizeof(ulint64));
        FLAGS_SET_SOLID(lwflags, xflags & G2FLAG_X_SOLID);
    }

    data_ptr = (byte *)geom->data;
    if (G2FLAGS_GET_EXTENDED(geom->gflags))
        data_ptr += sizeof(ulint64);

    if (FLAGS_GET_BBOX(lwflags))
    {
        if (FLAGS_GET_GEODETIC(geom->gflags))
            data_ptr += 6 * sizeof(float);
        else
            data_ptr += 2 * FLAGS_NDIMS(geom->gflags) * sizeof(float);
    }

    return lwgeom_from_gserialized_buffer(data_ptr, lwflags, srid);
}

/***************************************************************
Name:
dm_gser_to_wkb
Purpose:
    gserialized to wkb.
***************************************************************/
byte *dm_gser_to_wkb(GSERIALIZED *geom,
                     ulint64 *size,
                     ulint *type)
{
    LWGEOM *lwgeom;
    byte *wkb;

    if (!geom)
        return NULL;

    lwgeom = lwgeom_from_gserialized(geom);

    if (!lwgeom)
        return NULL; /* Ooops! */

    wkb = lwgeom_to_wkb_buffer(lwgeom, WKB_EXTENDED, size);
    lwgeom_free(lwgeom);

    return wkb;
}

/**
* Internal function declarations.
*/
LWGEOM *lwgeom_from_wkb_state(wkb_parse_state *s);

/* Our static character->number map. Anything > 15 is invalid */
static unsigned char hex2char[256] = {
    /* not Hex characters */
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    /* 0-9 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 20, 20, 20, 20, 20, 20,
    /* A-F */
    20, 10, 11, 12, 13, 14, 15, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    /* not Hex characters */
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    /* a-f */
    20, 10, 11, 12, 13, 14, 15, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    /* not Hex characters (upper 128 characters) */
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20};

unsigned char *bytes_from_hexbytes(const char *hexbuf,
                                   size_t hexsize)
{
    unsigned char *buf = NULL;
    register unsigned char h1, h2;
    uint32_t i;

    if (hexsize % 2)
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid hex string, length (%d) has to be a multiple of two!",
                 hexsize);

    buf = (unsigned char *)CPLMalloc(hexsize / 2);

    if (!buf)
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to allocate memory buffer.");

    for (i = 0; i < hexsize / 2; i++)
    {
        h1 = hex2char[(int)hexbuf[2 * i]];
        h2 = hex2char[(int)hexbuf[2 * i + 1]];
        if (h1 > 15)
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid hex character (%c) encountered", hexbuf[2 * i]);
        if (h2 > 15)
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid hex character (%c) encountered",
                     hexbuf[2 * i + 1]);
        /* First character is high bits, second is low bits */
        buf[i] = ((h1 & 0x0F) << 4) | (h2 & 0x0F);
    }
    return buf;
}

/**
* Check that we are not about to read off the end of the WKB
* array.
*/
static inline void wkb_parse_state_check(wkb_parse_state *s,
                                         size_t next)
{
    if ((s->pos + next) > (s->wkb + s->wkb_size))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "WKB structure does not match expected size!");
        s->error = LW_TRUE;
    }
}

/**
* Take in an unknown kind of wkb type number and ensure it comes out
* as an extended WKB type number (with Z/M/SRID flags masked onto the
* high bits).
*/
static void lwtype_from_wkb_state(wkb_parse_state *s,
                                  uint32_t wkb_type)
{
    uint32_t wkb_simple_type;

    s->has_z = LW_FALSE;
    s->has_m = LW_FALSE;
    s->has_srid = LW_FALSE;

    /* If any of the higher bits are set, this is probably an extended type. */
    if (wkb_type & 0xF0000000)
    {
        if (wkb_type & WKBZOFFSET)
            s->has_z = LW_TRUE;
        if (wkb_type & WKBMOFFSET)
            s->has_m = LW_TRUE;
        if (wkb_type & WKBSRIDFLAG)
            s->has_srid = LW_TRUE;
    }

    /* Mask off the flags */
    wkb_type = wkb_type & 0x0FFFFFFF;

    /* Catch strange Oracle WKB type numbers */
    if (wkb_type >= 4000)
    {
        return;
    }

    /* Strip out just the type number (1-12) from the ISO number (eg 3001-3012) */
    wkb_simple_type = wkb_type % 1000;

    /* Extract the Z/M information from ISO style numbers */
    if (wkb_type >= 3000 && wkb_type < 4000)
    {
        s->has_z = LW_TRUE;
        s->has_m = LW_TRUE;
    }
    else if (wkb_type >= 2000 && wkb_type < 3000)
    {
        s->has_m = LW_TRUE;
    }
    else if (wkb_type >= 1000 && wkb_type < 2000)
    {
        s->has_z = LW_TRUE;
    }

    switch (wkb_simple_type)
    {
        case WKB_POINT_TYPE:
            s->lwtype = POINTTYPE;
            break;
        case WKB_LINESTRING_TYPE:
            s->lwtype = LINETYPE;
            break;
        case WKB_POLYGON_TYPE:
            s->lwtype = POLYGONTYPE;
            break;
        case WKB_MULTIPOINT_TYPE:
            s->lwtype = MULTIPOINTTYPE;
            break;
        case WKB_MULTILINESTRING_TYPE:
            s->lwtype = MULTILINETYPE;
            break;
        case WKB_MULTIPOLYGON_TYPE:
            s->lwtype = MULTIPOLYGONTYPE;
            break;
        case WKB_GEOMETRYCOLLECTION_TYPE:
            s->lwtype = COLLECTIONTYPE;
            break;
        case WKB_CIRCULARSTRING_TYPE:
            s->lwtype = CIRCSTRINGTYPE;
            break;
        case WKB_COMPOUNDCURVE_TYPE:
            s->lwtype = COMPOUNDTYPE;
            break;
        case WKB_CURVEPOLYGON_TYPE:
            s->lwtype = CURVEPOLYTYPE;
            break;
        case WKB_MULTICURVE_TYPE:
            s->lwtype = MULTICURVETYPE;
            break;
        case WKB_MULTISURFACE_TYPE:
            s->lwtype = MULTISURFACETYPE;
            break;
        case WKB_POLYHEDRALSURFACE_TYPE:
            s->lwtype = POLYHEDRALSURFACETYPE;
            break;
        case WKB_TIN_TYPE:
            s->lwtype = TINTYPE;
            break;
        case WKB_TRIANGLE_TYPE:
            s->lwtype = TRIANGLETYPE;
            break;

            /* DMGEO2 emits 13, 14 for CurvePolygon, MultiCurve */
            /* These numbers aren't SQL/MM (numbers currently only */
            /* go up to 12. We can handle the old data here (for now??) */
            /* converting them into the lwtypes that are intended. */
        case WKB_CURVE_TYPE:
            s->lwtype = CURVEPOLYTYPE;
            break;
        case WKB_SURFACE_TYPE:
            s->lwtype = MULTICURVETYPE;
            break;

        default: /* Error! */
            break;
    }

    return;
}

/**
* Byte
* Read a byte and advance the parse state forward.
*/
static char byte_from_wkb_state(wkb_parse_state *s)
{
    char char_value = 0;

    wkb_parse_state_check(s, WKB_BYTE_SIZE);
    if (s->error)
        return 0;

    char_value = s->pos[0];
    s->pos += WKB_BYTE_SIZE;

    return char_value;
}

/**
* Int32
* Read 4-byte integer and advance the parse state forward.
*/
static uint32_t integer_from_wkb_state(wkb_parse_state *s)
{
    uint32_t i = 0;

    wkb_parse_state_check(s, WKB_INT_SIZE);
    if (s->error)
        return 0;

    memcpy(&i, s->pos, WKB_INT_SIZE);

    /* Swap? Copy into a stack-allocated integer. */
    if (s->swap_bytes)
    {
        int j = 0;
        unsigned char tmp;

        for (j = 0; j < WKB_INT_SIZE / 2; j++)
        {
            tmp = ((unsigned char *)(&i))[j];
            ((unsigned char *)(&i))[j] =
                ((unsigned char *)(&i))[WKB_INT_SIZE - j - 1];
            ((unsigned char *)(&i))[WKB_INT_SIZE - j - 1] = tmp;
        }
    }

    s->pos += WKB_INT_SIZE;
    return i;
}

/**
* Double
* Read an 8-byte double and advance the parse state forward.
*/
static double double_from_wkb_state(wkb_parse_state *s)
{
    double d = 0;

    memcpy(&d, s->pos, WKB_DOUBLE_SIZE);

    /* Swap? Copy into a stack-allocated integer. */
    if (s->swap_bytes)
    {
        int i = 0;
        unsigned char tmp;

        for (i = 0; i < WKB_DOUBLE_SIZE / 2; i++)
        {
            tmp = ((unsigned char *)(&d))[i];
            ((unsigned char *)(&d))[i] =
                ((unsigned char *)(&d))[WKB_DOUBLE_SIZE - i - 1];
            ((unsigned char *)(&d))[WKB_DOUBLE_SIZE - i - 1] = tmp;
        }
    }

    s->pos += WKB_DOUBLE_SIZE;
    return d;
}

/*
 * Size of point represeneted in the POINTARRAY
 * 16 for 2d, 24 for 3d, 32 for 4d
 */
static inline size_t ptarray_point_size(const POINTARRAY *pa)
{
    return sizeof(double) * FLAGS_NDIMS(pa->flags);
}

POINTARRAY *ptarray_construct_copy_data(char hasz,
                                        char hasm,
                                        uint32_t npoints,
                                        const uint8_t *ptlist)
{
    POINTARRAY *pa = (POINTARRAY *)CPLMalloc(sizeof(POINTARRAY));

    pa->flags = ogr_lwflags(hasz, hasm, 0);
    pa->npoints = npoints;
    pa->maxpoints = npoints;

    if (npoints > 0)
    {
        pa->serialized_pointlist =
            (byte *)CPLMalloc(ptarray_point_size(pa) * npoints);
        memcpy(pa->serialized_pointlist, ptlist,
               ptarray_point_size(pa) * npoints);
    }
    else
    {
        pa->serialized_pointlist = NULL;
    }

    return pa;
}

/**
* POINTARRAY
* Read a dynamically sized point array and advance the parse state forward.
* First read the number of points, then read the points.
*/
static POINTARRAY *ptarray_from_wkb_state(wkb_parse_state *s)
{
    POINTARRAY *pa = NULL;
    size_t pa_size;
    uint32_t ndims = 2;
    uint32_t npoints = 0;
    static uint32_t maxpoints = UINT_MAX / WKB_DOUBLE_SIZE / 4;

    /* Calculate the size of this point array. */
    npoints = integer_from_wkb_state(s);
    if (s->error)
        return NULL;

    if (npoints > maxpoints)
    {
        s->error = LW_TRUE;
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Pointarray length (%d) is too large", npoints);
        return NULL;
    }

    if (s->has_z)
        ndims++;
    if (s->has_m)
        ndims++;
    pa_size = npoints * ndims * WKB_DOUBLE_SIZE;

    /* Empty! */
    if (npoints == 0)
        return ptarray_construct(s->has_z, s->has_m, npoints);

    /* Does the data we want to read exist? */
    wkb_parse_state_check(s, pa_size);
    if (s->error)
        return NULL;

    /* If we're in a native endianness, we can just copy the data directly! */
    if (!s->swap_bytes)
    {
        pa = ptarray_construct_copy_data(s->has_z, s->has_m, npoints,
                                         (unsigned char *)s->pos);
        s->pos += pa_size;
    }
    /* Otherwise we have to read each double, separately. */
    else
    {
        uint32_t i = 0;
        double *dlist;
        pa = ptarray_construct(s->has_z, s->has_m, npoints);
        dlist = (double *)(pa->serialized_pointlist);
        for (i = 0; i < npoints * ndims; i++)
        {
            dlist[i] = double_from_wkb_state(s);
        }
    }

    return pa;
}

/*
 * Get a pointer to Nth point of a POINTARRAY
 * You'll need to cast it to appropriate dimensioned point.
 * Note that if you cast to a higher dimensional point you'll
 * possibly corrupt the POINTARRAY.
 *
 * Casting to returned pointer to POINT2D* should be safe,
 * as gserialized format always keeps the POINTARRAY pointer
 * aligned to double boundary.
 *
 * WARNING: Don't cast this to a POINT!
 * it would not be reliable due to memory alignment constraints
 */
static inline uint8_t *getPoint_internal(const POINTARRAY *pa,
                                         uint32_t n)
{
    size_t size;
    uint8_t *ptr;

    size = ptarray_point_size(pa);
    ptr = pa->serialized_pointlist + size * n;

    return ptr;
}

/**
 * Returns a POINT2D pointer into the POINTARRAY serialized_ptlist,
 * suitable for reading from. This is very high performance
 * and declared const because you aren't allowed to muck with the
 * values, only read them.
 */
static inline const POINT2D *getPoint2d_cp(const POINTARRAY *pa,
                                           uint32_t n)
{
    return (const POINT2D *)getPoint_internal(pa, n);
}

LWPOINT *lwpoint_construct_empty(int32_t srid,
                                 char hasz,
                                 char hasm)
{
    LWPOINT *result = (LWPOINT *)CPLMalloc(sizeof(LWPOINT));
    result->type = POINTTYPE;
    result->flags = ogr_lwflags(hasz, hasm, 0);
    result->srid = srid;
    result->point = ptarray_construct(hasz, hasm, 0);
    result->bbox = NULL;
    return result;
}

/**
* POINT
* Read a WKB point, starting just after the endian byte,
* type number and optional srid number.
* Advance the parse state forward appropriately.
* WKB point has just a set of doubles, with the quantity depending on the
* dimension of the point, so this looks like a special case of the above
* with only one point.
*/
static LWPOINT *lwpoint_from_wkb_state(wkb_parse_state *s)
{
    static uint32_t npoints = 1;
    POINTARRAY *pa = NULL;
    size_t pa_size;
    uint32_t ndims = 2;
    const POINT2D *pt;

    /* Count the dimensions. */
    if (s->has_z)
        ndims++;
    if (s->has_m)
        ndims++;
    pa_size = ndims * WKB_DOUBLE_SIZE;

    /* Does the data we want to read exist? */
    wkb_parse_state_check(s, pa_size);
    if (s->error)
        return NULL;

    /* If we're in a native endianness, we can just copy the data directly! */
    if (!s->swap_bytes)
    {
        pa = ptarray_construct_copy_data(s->has_z, s->has_m, npoints,
                                         (unsigned char *)s->pos);
        s->pos += pa_size;
    }
    /* Otherwise we have to read each double, separately */
    else
    {
        uint32_t i = 0;
        double *dlist;
        pa = ptarray_construct(s->has_z, s->has_m, npoints);
        dlist = (double *)(pa->serialized_pointlist);
        for (i = 0; i < ndims; i++)
        {
            dlist[i] = double_from_wkb_state(s);
        }
    }

    /* Check for POINT(NaN NaN) ==> POINT EMPTY */
    pt = getPoint2d_cp(pa, 0);
    if (isnan(pt->x) && isnan(pt->y))
    {
        ptarray_free(pa);
        return lwpoint_construct_empty(s->srid, s->has_z, s->has_m);
    }
    else
    {
        return lwpoint_construct(s->srid, NULL, pa);
    }
}

LWLINE *lwline_construct_empty(int32_t srid,
                               char hasz,
                               char hasm)
{
    LWLINE *result = (LWLINE *)CPLMalloc(sizeof(LWLINE));
    result->type = LINETYPE;
    result->flags = ogr_lwflags(hasz, hasm, 0);
    result->srid = srid;
    result->points = ptarray_construct_empty(hasz, hasm, 1);
    result->bbox = NULL;
    return result;
}

/*
 * Construct a new LWLINE.  points will *NOT* be copied
 * use SRID=SRID_UNKNOWN for unknown SRID (will have 8bit type's S = 0)
 */
LWLINE *lwline_construct(int32_t srid,
                         GBOX *bbox,
                         POINTARRAY *points)
{
    LWLINE *result = (LWLINE *)CPLMalloc(sizeof(LWLINE));
    result->type = LINETYPE;
    result->flags = points->flags;
    FLAGS_SET_BBOX(result->flags, bbox ? 1 : 0);
    result->srid = srid;
    result->points = points;
    result->bbox = bbox;
    return result;
}

/**
* LINESTRING
* Read a WKB linestring, starting just after the endian byte,
* type number and optional srid number. Advance the parse state
* forward appropriately.
* There is only one pointarray in a linestring. Optionally
* check for minimal following of rules (two point minimum).
*/
static LWLINE *lwline_from_wkb_state(wkb_parse_state *s)
{
    POINTARRAY *pa = ptarray_from_wkb_state(s);
    if (s->error)
        return NULL;

    if (pa == NULL || pa->npoints == 0)
    {
        if (pa)
            ptarray_free(pa);
        return lwline_construct_empty(s->srid, s->has_z, s->has_m);
    }

    if (s->check & LW_PARSER_CHECK_MINPOINTS && pa->npoints < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "must have at least two points");
        return NULL;
    }

    return lwline_construct(s->srid, NULL, pa);
}

LWCIRCSTRING *lwcircstring_construct_empty(int32_t srid,
                                           char hasz,
                                           char hasm)
{
    LWCIRCSTRING *result = (LWCIRCSTRING *)CPLMalloc(sizeof(LWCIRCSTRING));
    result->type = CIRCSTRINGTYPE;
    result->flags = ogr_lwflags(hasz, hasm, 0);
    result->srid = srid;
    result->points = ptarray_construct_empty(hasz, hasm, 1);
    result->bbox = NULL;
    return result;
}

/*
 * Construct a new LWCIRCSTRING.  points will *NOT* be copied
 * use SRID=SRID_UNKNOWN for unknown SRID (will have 8bit type's S = 0)
 */
LWCIRCSTRING *lwcircstring_construct(int32_t srid,
                                     GBOX *bbox,
                                     POINTARRAY *points)
{
    LWCIRCSTRING *result;

    result = (LWCIRCSTRING *)CPLMalloc(sizeof(LWCIRCSTRING));

    result->type = CIRCSTRINGTYPE;

    result->flags = points->flags;
    FLAGS_SET_BBOX(result->flags, bbox ? 1 : 0);

    result->srid = srid;
    result->points = points;
    result->bbox = bbox;

    return result;
}

/**
* CIRCULARSTRING
* Read a WKB circularstring, starting just after the endian byte,
* type number and optional srid number. Advance the parse state
* forward appropriately.
* There is only one pointarray in a linestring. Optionally
* check for minimal following of rules (three point minimum,
* odd number of points).
*/
static LWCIRCSTRING *lwcircstring_from_wkb_state(wkb_parse_state *s)
{
    POINTARRAY *pa = ptarray_from_wkb_state(s);
    if (s->error)
        return NULL;

    if (pa == NULL || pa->npoints == 0)
    {
        if (pa)
            ptarray_free(pa);
        return lwcircstring_construct_empty(s->srid, s->has_z, s->has_m);
    }

    if (s->check & LW_PARSER_CHECK_MINPOINTS && pa->npoints < 3)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "must have at least three points");
        return NULL;
    }

    if (s->check & LW_PARSER_CHECK_ODD && !(pa->npoints % 2))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "must have an odd number of points");
        return NULL;
    }

    return lwcircstring_construct(s->srid, NULL, pa);
}

LWPOLY *lwpoly_construct_empty(int32_t srid,
                               char hasz,
                               char hasm)
{
    LWPOLY *result = (LWPOLY *)CPLMalloc(sizeof(LWPOLY));
    result->type = POLYGONTYPE;
    result->flags = ogr_lwflags(hasz, hasm, 0);
    result->srid = srid;
    result->nrings = 0;
    result->maxrings = 1; /* Allocate room for ring, just in case. */
    result->rings =
        (POINTARRAY **)CPLMalloc(result->maxrings * sizeof(POINTARRAY *));
    result->bbox = NULL;
    return result;
}

/**
* Add a ring to a polygon. Point array will be referenced, not copied.
*/
int lwpoly_add_ring(LWPOLY *poly,
                    POINTARRAY *pa)
{
    if (!poly || !pa)
        return LW_FAILURE;

    /* We have used up our storage, add some more. */
    if (poly->nrings >= poly->maxrings)
    {
        int new_maxrings = 2 * (poly->nrings + 1);
        poly->rings = (POINTARRAY **)CPLRealloc(
            poly->rings, new_maxrings * sizeof(POINTARRAY *));
        poly->maxrings = new_maxrings;
    }

    /* Add the new ring entry. */
    poly->rings[poly->nrings] = pa;
    poly->nrings++;

    return LW_SUCCESS;
}

int ptarray_is_closed_2d(const POINTARRAY *in)
{
    if (!in)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ptarray_is_closed_2d: called with null point array");
        return 0;
    }
    if (in->npoints <= 1)
        return in->npoints; /* single-point are closed, empty not closed */

    return 0 == memcmp(getPoint_internal(in, 0),
                       getPoint_internal(in, in->npoints - 1), sizeof(POINT2D));
}

/**
* POLYGON
* Read a WKB polygon, starting just after the endian byte,
* type number and optional srid number. Advance the parse state
* forward appropriately.
* First read the number of rings, then read each ring
* (which are structured as point arrays)
*/
static LWPOLY *lwpoly_from_wkb_state(wkb_parse_state *s)
{
    uint32_t nrings = integer_from_wkb_state(s);
    if (s->error)
        return NULL;
    uint32_t i = 0;
    LWPOLY *poly = lwpoly_construct_empty(s->srid, s->has_z, s->has_m);

    /* Empty polygon? */
    if (nrings == 0)
        return poly;

    for (i = 0; i < nrings; i++)
    {
        POINTARRAY *pa = ptarray_from_wkb_state(s);
        if (pa == NULL)
        {
            lwpoly_free(poly);
            return NULL;
        }

        /* Check for at least four points. */
        if (s->check & LW_PARSER_CHECK_MINPOINTS && pa->npoints < 4)
        {
            lwpoly_free(poly);
            ptarray_free(pa);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "must have at least four points in each ring");
            return NULL;
        }

        /* Check that first and last points are the same. */
        if (s->check & LW_PARSER_CHECK_CLOSURE && !ptarray_is_closed_2d(pa))
        {
            lwpoly_free(poly);
            ptarray_free(pa);
            CPLError(CE_Failure, CPLE_AppDefined, "must have closed rings");
            return NULL;
        }

        /* Add ring to polygon */
        if (lwpoly_add_ring(poly, pa) == LW_FAILURE)
        {
            lwpoly_free(poly);
            ptarray_free(pa);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to add ring to polygon");
            return NULL;
        }
    }
    return poly;
}

LWTRIANGLE *lwtriangle_construct_empty(int32_t srid,
                                       char hasz,
                                       char hasm)
{
    LWTRIANGLE *result = (LWTRIANGLE *)CPLMalloc(sizeof(LWTRIANGLE));
    result->type = TRIANGLETYPE;
    result->flags = ogr_lwflags(hasz, hasm, 0);
    result->srid = srid;
    result->points = ptarray_construct_empty(hasz, hasm, 1);
    result->bbox = NULL;
    return result;
}

/* construct a new LWTRIANGLE.
 * use SRID=SRID_UNKNOWN for unknown SRID (will have 8bit type's S = 0)
 */
LWTRIANGLE *lwtriangle_construct(int32_t srid,
                                 GBOX *bbox,
                                 POINTARRAY *points)
{
    LWTRIANGLE *result;

    result = (LWTRIANGLE *)CPLMalloc(sizeof(LWTRIANGLE));
    result->type = TRIANGLETYPE;

    result->flags = points->flags;
    FLAGS_SET_BBOX(result->flags, bbox ? 1 : 0);

    result->srid = srid;
    result->points = points;
    result->bbox = bbox;

    return result;
}

int ptarray_is_closed_3d(const POINTARRAY *in)
{
    if (!in)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ptarray_is_closed_3d: called with null point array");
        return 0;
    }
    if (in->npoints <= 1)
        return in->npoints; /* single-point are closed, empty not closed */

    return 0 == memcmp(getPoint_internal(in, 0),
                       getPoint_internal(in, in->npoints - 1), sizeof(POINT3D));
}

int ptarray_is_closed_z(const POINTARRAY *in)
{
    if (FLAGS_GET_Z(in->flags))
        return ptarray_is_closed_3d(in);
    else
        return ptarray_is_closed_2d(in);
}

/**
* TRIANGLE
* Read a WKB triangle, starting just after the endian byte,
* type number and optional srid number. Advance the parse state
* forward appropriately.
* Triangles are encoded like polygons in WKB, but more like linestrings
* as lwgeometries.
*/
static LWTRIANGLE *lwtriangle_from_wkb_state(wkb_parse_state *s)
{
    uint32_t nrings = integer_from_wkb_state(s);
    if (s->error)
        return NULL;

    /* Empty triangle? */
    if (nrings == 0)
        return lwtriangle_construct_empty(s->srid, s->has_z, s->has_m);

    /* Should be only one ring. */
    if (nrings != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Triangle has wrong number of rings: %d", nrings);
    }

    /* There's only one ring, we hope? */
    POINTARRAY *pa = ptarray_from_wkb_state(s);

    /* If there's no points, return an empty triangle. */
    if (pa == NULL)
        return lwtriangle_construct_empty(s->srid, s->has_z, s->has_m);

    /* Check for at least four points. */
    if (s->check & LW_PARSER_CHECK_MINPOINTS && pa->npoints < 4)
    {
        ptarray_free(pa);
        CPLError(CE_Failure, CPLE_AppDefined, "must have at least four points");
        return NULL;
    }

    if (s->check & LW_PARSER_CHECK_ZCLOSURE && !ptarray_is_closed_z(pa))
    {
        ptarray_free(pa);
        CPLError(CE_Failure, CPLE_AppDefined, "must have closed rings");
        return NULL;
    }

    /* Empty TRIANGLE starts w/ empty POINTARRAY, free it first */
    return lwtriangle_construct(s->srid, NULL, pa);
}

LWCURVEPOLY *lwcurvepoly_construct_empty(int32_t srid,
                                         char hasz,
                                         char hasm)
{
    LWCURVEPOLY *ret;

    ret = (LWCURVEPOLY *)CPLMalloc(sizeof(LWCURVEPOLY));
    ret->type = CURVEPOLYTYPE;
    ret->flags = ogr_lwflags(hasz, hasm, 0);
    ret->srid = srid;
    ret->nrings = 0;
    ret->maxrings = 1; /* Allocate room for sub-members, just in case. */
    ret->rings = (LWGEOM **)CPLMalloc(ret->maxrings * sizeof(LWGEOM *));
    ret->bbox = NULL;

    return ret;
}

int lwcurvepoly_add_ring(LWCURVEPOLY *poly,
                         LWGEOM *ring)
{
    uint32_t i;

    /* Can't do anything with NULLs */
    if (!poly || !ring)
    {
        return LW_FAILURE;
    }

    /* Check that we're not working with garbage */
    if (poly->rings == NULL && (poly->nrings || poly->maxrings))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Curvepolygon is in inconsistent state. Null memory but "
                 "non-zero collection counts.");
        return LW_FAILURE;
    }

    /* Check that we're adding an allowed ring type */
    if (!(ring->type == LINETYPE || ring->type == CIRCSTRINGTYPE ||
          ring->type == COMPOUNDTYPE))
    {
        return LW_FAILURE;
    }

    /* In case this is a truly empty, make some initial space  */
    if (poly->rings == NULL)
    {
        poly->maxrings = 2;
        poly->nrings = 0;
        poly->rings = (LWGEOM **)CPLMalloc(poly->maxrings * sizeof(LWGEOM *));
    }

    /* Allocate more space if we need it */
    if (poly->nrings == poly->maxrings)
    {
        poly->maxrings *= 2;
        poly->rings = (LWGEOM **)CPLRealloc(poly->rings,
                                            sizeof(LWGEOM *) * poly->maxrings);
    }

    /* Make sure we don't already have a reference to this geom */
    for (i = 0; i < poly->nrings; i++)
    {
        if (poly->rings[i] == ring)
        {
            return LW_SUCCESS;
        }
    }

    /* Add the ring and increment the ring count */
    poly->rings[poly->nrings] = (LWGEOM *)ring;
    poly->nrings++;
    return LW_SUCCESS;
}

/**
* CURVEPOLYTYPE
*/
static LWCURVEPOLY *lwcurvepoly_from_wkb_state(wkb_parse_state *s)
{
    uint32_t ngeoms = integer_from_wkb_state(s);
    if (s->error)
        return NULL;
    LWCURVEPOLY *cp = lwcurvepoly_construct_empty(s->srid, s->has_z, s->has_m);
    LWGEOM *geom = NULL;
    uint32_t i;

    /* Empty collection? */
    if (ngeoms == 0)
        return cp;

    for (i = 0; i < ngeoms; i++)
    {
        geom = lwgeom_from_wkb_state(s);
        if (lwcurvepoly_add_ring(cp, geom) == LW_FAILURE)
        {
            lwgeom_free(geom);
            lwgeom_free((LWGEOM *)cp);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to add geometry (%p) to curvepoly (%p)", geom, cp);
            return NULL;
        }
    }

    return cp;
}

/**
* POLYHEDRALSURFACETYPE
*/

/** Return TRUE if the geometry may contain sub-geometries, i.e. it is a MULTI* or COMPOUNDCURVE */
int lwtype_is_collection(uint8_t type)
{
    switch (type)
    {
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case COLLECTIONTYPE:
        case CURVEPOLYTYPE:
        case COMPOUNDTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
            return LW_TRUE;
            break;

        default:
            return LW_FALSE;
    }
}

LWCOLLECTION *lwcollection_construct_empty(uint8_t type,
                                           int32_t srid,
                                           char hasz,
                                           char hasm)
{
    LWCOLLECTION *ret;
    if (!lwtype_is_collection(type))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Non-collection type specified in collection constructor!");
        return NULL;
    }

    ret = (LWCOLLECTION *)CPLMalloc(sizeof(LWCOLLECTION));
    ret->type = type;
    ret->flags = ogr_lwflags(hasz, hasm, 0);
    ret->srid = srid;
    ret->ngeoms = 0;
    ret->maxgeoms = 1; /* Allocate room for sub-members, just in case. */
    ret->geoms = (LWGEOM **)CPLMalloc(ret->maxgeoms * sizeof(LWGEOM *));
    ret->bbox = NULL;

    return ret;
}

/**
 * Ensure the collection can hold up at least ngeoms
 */
void lwcollection_reserve(LWCOLLECTION *col,
                          uint32_t ngeoms)
{
    if (ngeoms <= col->maxgeoms)
        return;

    /* Allocate more space if we need it */
    do
    {
        col->maxgeoms *= 2;
    } while (col->maxgeoms < ngeoms);
    col->geoms =
        (LWGEOM **)CPLRealloc(col->geoms, sizeof(LWGEOM *) * col->maxgeoms);
}

/**
* Appends geom to the collection managed by col. Does not copy or
* clone, simply takes a reference on the passed geom.
*/
LWCOLLECTION *lwcollection_add_lwgeom(LWCOLLECTION *col,
                                      const LWGEOM *geom)
{
    if (!col || !geom)
        return NULL;

    if (!col->geoms && (col->ngeoms || col->maxgeoms))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Collection is in inconsistent state. Null memory but "
                 "non-zero collection counts.");
        return NULL;
    }

    /* Check type compatibility */
    if (!lwcollection_allows_subtype(col->type, geom->type))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Collection cannot contain this element");
        return NULL;
    }

    /* In case this is a truly empty, make some initial space  */
    if (!col->geoms)
    {
        col->maxgeoms = 2;
        col->ngeoms = 0;
        col->geoms = (LWGEOM **)CPLMalloc(col->maxgeoms * sizeof(LWGEOM *));
    }

    /* Allocate more space if we need it */
    lwcollection_reserve(col, col->ngeoms + 1);

    col->geoms[col->ngeoms] = (LWGEOM *)geom;
    col->ngeoms++;
    return col;
}

/**
* COLLECTION, MULTIPOINTTYPE, MULTILINETYPE, MULTIPOLYGONTYPE, COMPOUNDTYPE,
* MULTICURVETYPE, MULTISURFACETYPE,
* TINTYPE
*/
static LWCOLLECTION *lwcollection_from_wkb_state(wkb_parse_state *s)
{
    uint32_t ngeoms = integer_from_wkb_state(s);
    if (s->error)
        return NULL;
    LWCOLLECTION *col =
        lwcollection_construct_empty(s->lwtype, s->srid, s->has_z, s->has_m);
    LWGEOM *geom = NULL;
    uint32_t i;

    /* Empty collection? */
    if (ngeoms == 0)
        return col;

    /* Be strict in polyhedral surface closures */
    if (s->lwtype == POLYHEDRALSURFACETYPE)
        s->check |= LW_PARSER_CHECK_ZCLOSURE;

    s->depth++;
    if (s->depth >= LW_PARSER_MAX_DEPTH)
    {
        lwcollection_free(col);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Geometry has too many chained collections");
        return NULL;
    }
    for (i = 0; i < ngeoms; i++)
    {
        geom = lwgeom_from_wkb_state(s);
        if (lwcollection_add_lwgeom(col, geom) == NULL)
        {
            lwgeom_free(geom);
            lwgeom_free((LWGEOM *)col);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to add geometry (%p) to collection (%p)", geom,
                     col);
            return NULL;
        }
    }
    s->depth--;

    return col;
}

int32_t clamp_srid(int32_t srid)
{
    int newsrid = srid;

    if (newsrid <= 0)
    {
        if (newsrid != SRID_UNKNOWN)
        {
            newsrid = SRID_UNKNOWN;
        }
    }
    else if (srid > SRID_MAXIMUM)
    {
        newsrid = SRID_USER_MAXIMUM + 1 +
                  (srid % (SRID_MAXIMUM - SRID_USER_MAXIMUM - 1));
        CPLError(CE_Warning, CPLE_AppDefined,
                 "SRID value %d > SRID_MAXIMUM converted to %d", srid, newsrid);
    }

    return newsrid;
}

/**
* GEOMETRY
* Generic handling for WKB geometries. The front of every WKB geometry
* (including those embedded in collections) is an endian byte, a type
* number and an optional srid number. We handle all those here, then pass
* to the appropriate handler for the specific type.
*/
LWGEOM *lwgeom_from_wkb_state(wkb_parse_state *s)
{
    char wkb_little_endian;
    uint32_t wkb_type;

    /* Fail when handed incorrect starting byte */
    wkb_little_endian = byte_from_wkb_state(s);
    if (s->error)
        return NULL;
    if (wkb_little_endian != 1 && wkb_little_endian != 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid endian flag value encountered.");
        return NULL;
    }

    /* Check the endianness of our input  */
    s->swap_bytes = LW_FALSE;

    /* Machine arch is big endian, request is for little */
    if (IS_BIG_ENDIAN && wkb_little_endian)
        s->swap_bytes = LW_TRUE;
    /* Machine arch is little endian, request is for big */
    else if ((!IS_BIG_ENDIAN) && (!wkb_little_endian))
        s->swap_bytes = LW_TRUE;

    /* Read the type number */
    wkb_type = integer_from_wkb_state(s);
    if (s->error)
        return NULL;
    lwtype_from_wkb_state(s, wkb_type);

    /* Read the SRID, if necessary */
    if (s->has_srid)
    {
        s->srid = clamp_srid(integer_from_wkb_state(s));
        if (s->error)
            return NULL;
    }

    /* Do the right thing */
    switch (s->lwtype)
    {
        case POINTTYPE:
            return (LWGEOM *)lwpoint_from_wkb_state(s);
            break;
        case LINETYPE:
            return (LWGEOM *)lwline_from_wkb_state(s);
            break;
        case CIRCSTRINGTYPE:
            return (LWGEOM *)lwcircstring_from_wkb_state(s);
            break;
        case POLYGONTYPE:
            return (LWGEOM *)lwpoly_from_wkb_state(s);
            break;
        case TRIANGLETYPE:
            return (LWGEOM *)lwtriangle_from_wkb_state(s);
            break;
        case CURVEPOLYTYPE:
            return (LWGEOM *)lwcurvepoly_from_wkb_state(s);
            break;
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case COMPOUNDTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return (LWGEOM *)lwcollection_from_wkb_state(s);
            break;

            /* Unknown type! */
        default:
            CPLError(CE_Failure, CPLE_AppDefined, "Unsupported geometry type");
    }

    /* Return value to keep compiler happy. */
    return NULL;
}

/* TODO add check for SRID consistency */

/**
* WKB inputs *must* have a declared size, to prevent malformed WKB from reading
* off the end of the memory segment (this stops a malevolent user from declaring
* a one-ring polygon to have 10 rings, causing the WKB reader to walk off the
* end of the memory).
*
* Check is a bitmask of: LW_PARSER_CHECK_MINPOINTS, LW_PARSER_CHECK_ODD,
* LW_PARSER_CHECK_CLOSURE, LW_PARSER_CHECK_NONE, LW_PARSER_CHECK_ALL
*/
LWGEOM *lwgeom_from_wkb(const unsigned char *wkb,
                        const size_t wkb_size,
                        const char check)
{
    wkb_parse_state s;

    /* Initialize the state appropriately */
    s.wkb = wkb;
    s.wkb_size = wkb_size;
    s.swap_bytes = LW_FALSE;
    s.check = check;
    s.lwtype = 0;
    s.srid = SRID_UNKNOWN;
    s.has_z = LW_FALSE;
    s.has_m = LW_FALSE;
    s.has_srid = LW_FALSE;
    s.error = LW_FALSE;
    s.pos = wkb;
    s.depth = 1;

    if (!wkb || !wkb_size)
        return NULL;

    return lwgeom_from_wkb_state(&s);
}

LWGEOM *lwgeom_from_hexwkb(const char *hexwkb,
                           const char check)
{
    size_t hexwkb_len;
    unsigned char *wkb;
    LWGEOM *lwgeom;

    if (!hexwkb)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "lwgeom_from_hexwkb: null input");
        return NULL;
    }

    hexwkb_len = strlen(hexwkb);
    wkb = bytes_from_hexbytes(hexwkb, hexwkb_len);
    lwgeom = lwgeom_from_wkb(wkb, hexwkb_len / 2, check);
    CPLFree(wkb);
    return lwgeom;
}

uint32_t lwline_count_vertices(const LWLINE *line)
{
    if (!line->points)
        return 0;
    return line->points->npoints;
}

uint32_t lwpoly_count_vertices(const LWPOLY *poly)
{
    uint32_t i = 0;
    uint32_t v = 0; /* vertices */
    for (i = 0; i < poly->nrings; i++)
    {
        v += poly->rings[i]->npoints;
    }
    return v;
}

uint32_t lwgeom_count_vertices(const LWGEOM *geom);

uint32_t lwcollection_count_vertices(const LWCOLLECTION *col)
{
    uint32_t i = 0;
    uint32_t v = 0; /* vertices */
    for (i = 0; i < col->ngeoms; i++)
    {
        v += lwgeom_count_vertices(col->geoms[i]);
    }
    return v;
}

/**
* Count points in an #LWGEOM.
* TODO: Make sure the internal functions don't overflow
*/
uint32_t lwgeom_count_vertices(const LWGEOM *geom)
{
    int result = 0;

    /* Null? Zero. */
    if (!geom)
        return 0;

    /* Empty? Zero. */
    if (lwgeom_is_empty(geom))
        return 0;

    switch (geom->type)
    {
        case POINTTYPE:
            result = 1;
            break;
        case TRIANGLETYPE:
        case CIRCSTRINGTYPE:
        case LINETYPE:
            result = lwline_count_vertices((const LWLINE *)geom);
            break;
        case POLYGONTYPE:
            result = lwpoly_count_vertices((const LWPOLY *)geom);
            break;
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
        case MULTICURVETYPE:
        case MULTISURFACETYPE:
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            result = lwcollection_count_vertices((const LWCOLLECTION *)geom);
            break;
        default:
            CPLError(CE_Failure, CPLE_AppDefined,
                     "unsupported input geometry type");
            break;
    }
    return result;
}

int lwgeom_needs_bbox(const LWGEOM *geom)
{
    if (geom->type == POINTTYPE)
    {
        return LW_FALSE;
    }
    else if (geom->type == LINETYPE)
    {
        if (lwgeom_count_vertices(geom) <= 2)
            return LW_FALSE;
        else
            return LW_TRUE;
    }
    else if (geom->type == MULTIPOINTTYPE)
    {
        if (((LWCOLLECTION *)geom)->ngeoms == 1)
            return LW_FALSE;
        else
            return LW_TRUE;
    }
    else if (geom->type == MULTILINETYPE)
    {
        if (((LWCOLLECTION *)geom)->ngeoms == 1 &&
            lwgeom_count_vertices(geom) <= 2)
            return LW_FALSE;
        else
            return LW_TRUE;
    }
    else
    {
        return LW_TRUE;
    }
}

void gbox_init(GBOX *gbox)
{
    memset(gbox, 0, sizeof(GBOX));
}

GBOX *gbox_new(lwflags_t flags)
{
    GBOX *g = (GBOX *)CPLMalloc(sizeof(GBOX));
    gbox_init(g);
    g->flags = flags;
    return g;
}

void ll2cart(const POINT2D *g,
             POINT3D *p)
{
    double x_rad = M_PI * g->x / 180.0;
    double y_rad = M_PI * g->y / 180.0;
    double cos_y_rad = cos(y_rad);
    p->x = cos_y_rad * cos(x_rad);
    p->y = cos_y_rad * sin(x_rad);
    p->z = sin(y_rad);
}

int gbox_init_point3d(const POINT3D *p,
                      GBOX *gbox)
{
    gbox->xmin = gbox->xmax = p->x;
    gbox->ymin = gbox->ymax = p->y;
    gbox->zmin = gbox->zmax = p->z;
    return LW_SUCCESS;
}

int gbox_merge_point3d(const POINT3D *p,
                       GBOX *gbox)
{
    if (gbox->xmin > p->x)
        gbox->xmin = p->x;
    if (gbox->ymin > p->y)
        gbox->ymin = p->y;
    if (gbox->zmin > p->z)
        gbox->zmin = p->z;
    if (gbox->xmax < p->x)
        gbox->xmax = p->x;
    if (gbox->ymax < p->y)
        gbox->ymax = p->y;
    if (gbox->zmax < p->z)
        gbox->zmax = p->z;
    return LW_SUCCESS;
}

int p3d_same(const POINT3D *p1,
             const POINT3D *p2)
{
    return FP_EQUALS(p1->x, p2->x) && FP_EQUALS(p1->y, p2->y) &&
           FP_EQUALS(p1->z, p2->z);
}

static double dot_product(const POINT3D *p1,
                          const POINT3D *p2)
{
    return (p1->x * p2->x) + (p1->y * p2->y) + (p1->z * p2->z);
}

void vector_sum(const POINT3D *a,
                const POINT3D *b,
                POINT3D *n)
{
    n->x = a->x + b->x;
    n->y = a->y + b->y;
    n->z = a->z + b->z;
    return;
}

void normalize(POINT3D *p)
{
    double d = sqrt(p->x * p->x + p->y * p->y + p->z * p->z);
    if (FP_IS_ZERO(d))
    {
        p->x = p->y = p->z = 0.0;
        return;
    }
    p->x = p->x / d;
    p->y = p->y / d;
    p->z = p->z / d;
    return;
}

static void vector_difference(const POINT3D *a,
                              const POINT3D *b,
                              POINT3D *n)
{
    n->x = a->x - b->x;
    n->y = a->y - b->y;
    n->z = a->z - b->z;
    return;
}

static void cross_product(const POINT3D *a,
                          const POINT3D *b,
                          POINT3D *n)
{
    n->x = a->y * b->z - a->z * b->y;
    n->y = a->z * b->x - a->x * b->z;
    n->z = a->x * b->y - a->y * b->x;
    return;
}

void unit_normal(const POINT3D *P1,
                 const POINT3D *P2,
                 POINT3D *normal)
{
    double p_dot = dot_product(P1, P2);
    POINT3D P3;

    /* If edge is really large, calculate a narrower equivalent angle A1/A3. */
    if (p_dot < 0)
    {
        vector_sum(P1, P2, &P3);
        normalize(&P3);
    }
    /* If edge is narrow, calculate a wider equivalent angle A1/A3. */
    else if (p_dot > 0.95)
    {
        vector_difference(P2, P1, &P3);
        normalize(&P3);
    }
    /* Just keep the current angle in A1/A3. */
    else
    {
        P3 = *P2;
    }

    /* Normals to the A-plane and B-plane */
    cross_product(P1, &P3, normal);
    normalize(normal);
}

int lw_segment_side(const POINT2D *p1,
                    const POINT2D *p2,
                    const POINT2D *q)
{
    double side =
        ((q->x - p1->x) * (p2->y - p1->y) - (p2->x - p1->x) * (q->y - p1->y));
    return SIGNUM(side);
}

static void normalize2d(POINT2D *p)
{
    double d = sqrt(p->x * p->x + p->y * p->y);
    if (FP_IS_ZERO(d))
    {
        p->x = p->y = 0.0;
        return;
    }
    p->x = p->x / d;
    p->y = p->y / d;
    return;
}

int edge_calculate_gbox(const POINT3D *A1,
                        const POINT3D *A2,
                        GBOX *gbox)
{
    POINT2D R1, R2, RX, O;
    POINT3D AN, A3;
    POINT3D X[6];
    int i, o_side;

    /* Initialize the box with the edge end points */
    gbox_init_point3d(A1, gbox);
    gbox_merge_point3d(A2, gbox);

    /* Zero length edge, just return! */
    if (p3d_same(A1, A2))
        return LW_SUCCESS;

    /* Error out on antipodal edge */
    if (FP_EQUALS(A1->x, -1 * A2->x) && FP_EQUALS(A1->y, -1 * A2->y) &&
        FP_EQUALS(A1->z, -1 * A2->z))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Antipodal (180 degrees long) edge detected!");
        return LW_FAILURE;
    }

    /* Create A3, a vector in the plane of A1/A2, orthogonal to A1  */
    unit_normal(A1, A2, &AN);
    unit_normal(&AN, A1, &A3);

    /* Project A1 and A2 into the 2-space formed by the plane A1/A3 */
    R1.x = 1.0;
    R1.y = 0.0;
    R2.x = dot_product(A2, A1);
    R2.y = dot_product(A2, &A3);

    /* Initialize our 3-space axis points (x+, x-, y+, y-, z+, z-) */
    memset(X, 0, sizeof(POINT3D) * 6);
    X[0].x = X[2].y = X[4].z = 1.0;
    X[1].x = X[3].y = X[5].z = -1.0;

    /* Initialize a 2-space origin point. */
    O.x = O.y = 0.0;
    /* What side of the line joining R1/R2 is O? */
    o_side = lw_segment_side(&R1, &R2, &O);

    /* Add any extrema! */
    for (i = 0; i < 6; i++)
    {
        /* Convert 3-space axis points to 2-space unit vectors */
        RX.x = dot_product(&(X[i]), A1);
        RX.y = dot_product(&(X[i]), &A3);
        normalize2d(&RX);

        /* Any axis end on the side of R1/R2 opposite the origin */
        /* is an extreme point in the arc, so we add the 3-space */
        /* version of the point on R1/R2 to the gbox */
        if (lw_segment_side(&R1, &R2, &RX) != o_side)
        {
            POINT3D Xn;
            Xn.x = RX.x * A1->x + RX.y * A3.x;
            Xn.y = RX.x * A1->y + RX.y * A3.y;
            Xn.z = RX.x * A1->z + RX.y * A3.z;

            gbox_merge_point3d(&Xn, gbox);
        }
    }

    return LW_SUCCESS;
}

void gbox_duplicate(const GBOX *original,
                    GBOX *duplicate)
{
    memcpy(duplicate, original, sizeof(GBOX));
}

int gbox_merge(const GBOX *new_box,
               GBOX *merge_box)
{
    if (FLAGS_GET_ZM(merge_box->flags) != FLAGS_GET_ZM(new_box->flags))
        return LW_FAILURE;

    if (new_box->xmin < merge_box->xmin)
        merge_box->xmin = new_box->xmin;
    if (new_box->ymin < merge_box->ymin)
        merge_box->ymin = new_box->ymin;
    if (new_box->xmax > merge_box->xmax)
        merge_box->xmax = new_box->xmax;
    if (new_box->ymax > merge_box->ymax)
        merge_box->ymax = new_box->ymax;

    if (FLAGS_GET_Z(merge_box->flags) || FLAGS_GET_GEODETIC(merge_box->flags))
    {
        if (new_box->zmin < merge_box->zmin)
            merge_box->zmin = new_box->zmin;
        if (new_box->zmax > merge_box->zmax)
            merge_box->zmax = new_box->zmax;
    }
    if (FLAGS_GET_M(merge_box->flags))
    {
        if (new_box->mmin < merge_box->mmin)
            merge_box->mmin = new_box->mmin;
        if (new_box->mmax > merge_box->mmax)
            merge_box->mmax = new_box->mmax;
    }

    return LW_SUCCESS;
}

int ptarray_calculate_gbox_geodetic(const POINTARRAY *pa,
                                    GBOX *gbox)
{
    uint32_t i;
    int first = LW_TRUE;
    const POINT2D *p;
    POINT3D A1, A2;
    GBOX edge_gbox;

    gbox_init(&edge_gbox);
    edge_gbox.flags = gbox->flags;

    if (pa->npoints == 0)
        return LW_FAILURE;

    if (pa->npoints == 1)
    {
        p = getPoint2d_cp(pa, 0);
        ll2cart(p, &A1);
        gbox->xmin = gbox->xmax = A1.x;
        gbox->ymin = gbox->ymax = A1.y;
        gbox->zmin = gbox->zmax = A1.z;
        return LW_SUCCESS;
    }

    p = getPoint2d_cp(pa, 0);
    ll2cart(p, &A1);

    for (i = 1; i < pa->npoints; i++)
    {

        p = getPoint2d_cp(pa, i);
        ll2cart(p, &A2);

        edge_calculate_gbox(&A1, &A2, &edge_gbox);

        /* Initialize the box */
        if (first)
        {
            gbox_duplicate(&edge_gbox, gbox);
            first = LW_FALSE;
        }
        /* Expand the box where necessary */
        else
        {
            gbox_merge(&edge_gbox, gbox);
        }

        A1 = A2;
    }

    return LW_SUCCESS;
}

static int lwpoint_calculate_gbox_geodetic(const LWPOINT *point,
                                           GBOX *gbox)
{
    return ptarray_calculate_gbox_geodetic(point->point, gbox);
}

static int lwline_calculate_gbox_geodetic(const LWLINE *line,
                                          GBOX *gbox)
{
    return ptarray_calculate_gbox_geodetic(line->points, gbox);
}

static int gbox_check_poles(GBOX *gbox)
{
    int rv = LW_FALSE;
    /* Z axis */
    if (gbox->xmin < 0.0 && gbox->xmax > 0.0 && gbox->ymin < 0.0 &&
        gbox->ymax > 0.0)
    {
        /* Extrema lean positive */
        if ((gbox->zmin > 0.0) && (gbox->zmax > 0.0))
        {
            gbox->zmax = 1.0;
        }
        /* Extrema lean negative */
        else if ((gbox->zmin < 0.0) && (gbox->zmax < 0.0))
        {
            gbox->zmin = -1.0;
        }
        /* Extrema both sides! */
        else
        {
            gbox->zmin = -1.0;
            gbox->zmax = 1.0;
        }
        rv = LW_TRUE;
    }

    /* Y axis */
    if (gbox->xmin < 0.0 && gbox->xmax > 0.0 && gbox->zmin < 0.0 &&
        gbox->zmax > 0.0)
    {
        if ((gbox->ymin > 0.0) && (gbox->ymax > 0.0))
        {
            gbox->ymax = 1.0;
        }
        else if ((gbox->ymin < 0.0) && (gbox->ymax < 0.0))
        {
            gbox->ymin = -1.0;
        }
        else
        {
            gbox->ymax = 1.0;
            gbox->ymin = -1.0;
        }
        rv = LW_TRUE;
    }

    /* X axis */
    if (gbox->ymin < 0.0 && gbox->ymax > 0.0 && gbox->zmin < 0.0 &&
        gbox->zmax > 0.0)
    {
        if ((gbox->xmin > 0.0) && (gbox->xmax > 0.0))
        {
            gbox->xmax = 1.0;
        }
        else if ((gbox->xmin < 0.0) && (gbox->xmax < 0.0))
        {
            gbox->xmin = -1.0;
        }
        else
        {
            gbox->xmax = 1.0;
            gbox->xmin = -1.0;
        }

        rv = LW_TRUE;
    }

    return rv;
}

static int lwpolygon_calculate_gbox_geodetic(const LWPOLY *poly,
                                             GBOX *gbox)
{
    GBOX ringbox;
    uint32_t i;
    int first = LW_TRUE;
    if (poly->nrings == 0)
        return LW_FAILURE;
    ringbox.flags = gbox->flags;
    for (i = 0; i < poly->nrings; i++)
    {
        if (ptarray_calculate_gbox_geodetic(poly->rings[i], &ringbox) ==
            LW_FAILURE)
            return LW_FAILURE;
        if (first)
        {
            gbox_duplicate(&ringbox, gbox);
            first = LW_FALSE;
        }
        else
        {
            gbox_merge(&ringbox, gbox);
        }
    }

    /* If the box wraps a poly, push that axis to the absolute min/max as appropriate */
    gbox_check_poles(gbox);

    return LW_SUCCESS;
}

static int lwtriangle_calculate_gbox_geodetic(const LWTRIANGLE *triangle,
                                              GBOX *gbox)
{
    return ptarray_calculate_gbox_geodetic(triangle->points, gbox);
}

GBOX *gbox_copy(const GBOX *box)
{
    GBOX *copy = (GBOX *)CPLMalloc(sizeof(GBOX));
    memcpy(copy, box, sizeof(GBOX));
    return copy;
}

int lwgeom_calculate_gbox_geodetic(const LWGEOM *geom,
                                   GBOX *gbox);

static int lwcollection_calculate_gbox_geodetic(const LWCOLLECTION *coll,
                                                GBOX *gbox)
{
    GBOX subbox = {0};
    uint32_t i;
    int result = LW_FAILURE;
    int first = LW_TRUE;
    if (coll->ngeoms == 0)
        return LW_FAILURE;

    subbox.flags = gbox->flags;

    for (i = 0; i < coll->ngeoms; i++)
    {
        if (lwgeom_calculate_gbox_geodetic((LWGEOM *)(coll->geoms[i]),
                                           &subbox) == LW_SUCCESS)
        {
            /* Keep a copy of the sub-bounding box for later */
            if (coll->geoms[i]->bbox)
                CPLFree(coll->geoms[i]->bbox);
            coll->geoms[i]->bbox = gbox_copy(&subbox);
            if (first)
            {
                gbox_duplicate(&subbox, gbox);
                first = LW_FALSE;
            }
            else
            {
                gbox_merge(&subbox, gbox);
            }
            result = LW_SUCCESS;
        }
    }
    return result;
}

int lwgeom_calculate_gbox_geodetic(const LWGEOM *geom,
                                   GBOX *gbox)
{
    int result = LW_FAILURE;

    /* Add a geodetic flag to the incoming gbox */
    gbox->flags =
        ogr_lwflags(FLAGS_GET_Z(geom->flags), FLAGS_GET_M(geom->flags), 1);

    switch (geom->type)
    {
        case POINTTYPE:
            result = lwpoint_calculate_gbox_geodetic((LWPOINT *)geom, gbox);
            break;
        case LINETYPE:
            result = lwline_calculate_gbox_geodetic((LWLINE *)geom, gbox);
            break;
        case POLYGONTYPE:
            result = lwpolygon_calculate_gbox_geodetic((LWPOLY *)geom, gbox);
            break;
        case TRIANGLETYPE:
            result =
                lwtriangle_calculate_gbox_geodetic((LWTRIANGLE *)geom, gbox);
            break;
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTIPOLYGONTYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            result = lwcollection_calculate_gbox_geodetic((LWCOLLECTION *)geom,
                                                          gbox);
            break;
        default:
            CPLError(CE_Failure, CPLE_AppDefined,
                     "lwgeom_calculate_gbox_geodetic: unsupported input "
                     "geometry type: %d",
                     geom->type);
            break;
    }
    return result;
}

static void ptarray_calculate_gbox_cartesian_2d(const POINTARRAY *pa,
                                                GBOX *gbox)
{
    const POINT2D *p = getPoint2d_cp(pa, 0);

    gbox->xmax = gbox->xmin = p->x;
    gbox->ymax = gbox->ymin = p->y;

    for (uint32_t i = 1; i < pa->npoints; i++)
    {
        p = getPoint2d_cp(pa, i);
        gbox->xmin = FP_MIN(gbox->xmin, p->x);
        gbox->xmax = FP_MAX(gbox->xmax, p->x);
        gbox->ymin = FP_MIN(gbox->ymin, p->y);
        gbox->ymax = FP_MAX(gbox->ymax, p->y);
    }
}

static inline const POINT3D *getPoint3d_cp(const POINTARRAY *pa,
                                           uint32_t n)
{
    return (const POINT3D *)getPoint_internal(pa, n);
}

static void ptarray_calculate_gbox_cartesian_3d(const POINTARRAY *pa,
                                                GBOX *gbox)
{
    const POINT3D *p = getPoint3d_cp(pa, 0);

    gbox->xmax = gbox->xmin = p->x;
    gbox->ymax = gbox->ymin = p->y;
    gbox->zmax = gbox->zmin = p->z;

    for (uint32_t i = 1; i < pa->npoints; i++)
    {
        p = getPoint3d_cp(pa, i);
        gbox->xmin = FP_MIN(gbox->xmin, p->x);
        gbox->xmax = FP_MAX(gbox->xmax, p->x);
        gbox->ymin = FP_MIN(gbox->ymin, p->y);
        gbox->ymax = FP_MAX(gbox->ymax, p->y);
        gbox->zmin = FP_MIN(gbox->zmin, p->z);
        gbox->zmax = FP_MAX(gbox->zmax, p->z);
    }
}

static inline const POINT4D *getPoint4d_cp(const POINTARRAY *pa,
                                           uint32_t n)
{
    return (const POINT4D *)getPoint_internal(pa, n);
}

static void ptarray_calculate_gbox_cartesian_4d(const POINTARRAY *pa,
                                                GBOX *gbox)
{
    const POINT4D *p = getPoint4d_cp(pa, 0);

    gbox->xmax = gbox->xmin = p->x;
    gbox->ymax = gbox->ymin = p->y;
    gbox->zmax = gbox->zmin = p->z;
    gbox->mmax = gbox->mmin = p->m;

    for (uint32_t i = 1; i < pa->npoints; i++)
    {
        p = getPoint4d_cp(pa, i);
        gbox->xmin = FP_MIN(gbox->xmin, p->x);
        gbox->xmax = FP_MAX(gbox->xmax, p->x);
        gbox->ymin = FP_MIN(gbox->ymin, p->y);
        gbox->ymax = FP_MAX(gbox->ymax, p->y);
        gbox->zmin = FP_MIN(gbox->zmin, p->z);
        gbox->zmax = FP_MAX(gbox->zmax, p->z);
        gbox->mmin = FP_MIN(gbox->mmin, p->m);
        gbox->mmax = FP_MAX(gbox->mmax, p->m);
    }
}

int ptarray_calculate_gbox_cartesian(const POINTARRAY *pa,
                                     GBOX *gbox)
{
    if (!pa || pa->npoints == 0)
        return LW_FAILURE;
    if (!gbox)
        return LW_FAILURE;

    int has_z = FLAGS_GET_Z(pa->flags);
    int has_m = FLAGS_GET_M(pa->flags);
    gbox->flags = ogr_lwflags(has_z, has_m, 0);
    int coordinates = 2 + has_z + has_m;

    switch (coordinates)
    {
        case 2:
        {
            ptarray_calculate_gbox_cartesian_2d(pa, gbox);
            break;
        }
        case 3:
        {
            if (has_z)
            {
                ptarray_calculate_gbox_cartesian_3d(pa, gbox);
            }
            else
            {
                double zmin = gbox->zmin;
                double zmax = gbox->zmax;
                ptarray_calculate_gbox_cartesian_3d(pa, gbox);
                gbox->mmin = gbox->zmin;
                gbox->mmax = gbox->zmax;
                gbox->zmin = zmin;
                gbox->zmax = zmax;
            }
            break;
        }
        default:
        {
            ptarray_calculate_gbox_cartesian_4d(pa, gbox);
            break;
        }
    }
    return LW_SUCCESS;
}

static int lwpoint_calculate_gbox_cartesian(LWPOINT *point,
                                            GBOX *gbox)
{
    if (!point)
        return LW_FAILURE;
    return ptarray_calculate_gbox_cartesian(point->point, gbox);
}

static int lwline_calculate_gbox_cartesian(LWLINE *line,
                                           GBOX *gbox)
{
    if (!line)
        return LW_FAILURE;
    return ptarray_calculate_gbox_cartesian(line->points, gbox);
}

static int lwtriangle_calculate_gbox_cartesian(LWTRIANGLE *triangle,
                                               GBOX *gbox)
{
    if (!triangle)
        return LW_FAILURE;
    return ptarray_calculate_gbox_cartesian(triangle->points, gbox);
}

static int lwpoly_calculate_gbox_cartesian(LWPOLY *poly,
                                           GBOX *gbox)
{
    if (!poly)
        return LW_FAILURE;
    if (poly->nrings == 0)
        return LW_FAILURE;
    /* Just need to check outer ring */
    return ptarray_calculate_gbox_cartesian(poly->rings[0], gbox);
}

int lwgeom_calculate_gbox_cartesian(const LWGEOM *lwgeom,
                                    GBOX *gbox);

static int lwcollection_calculate_gbox_cartesian(LWCOLLECTION *coll,
                                                 GBOX *gbox)
{
    GBOX subbox = {0};
    uint32_t i;
    int result = LW_FAILURE;
    int first = LW_TRUE;
    if ((coll->ngeoms == 0) || !gbox)
        return LW_FAILURE;

    subbox.flags = coll->flags;

    for (i = 0; i < coll->ngeoms; i++)
    {
        if (lwgeom_calculate_gbox_cartesian((LWGEOM *)(coll->geoms[i]),
                                            &subbox) == LW_SUCCESS)
        {
            /* Keep a copy of the sub-bounding box for later
            if ( coll->geoms[i]->bbox )
                lwfree(coll->geoms[i]->bbox);
            coll->geoms[i]->bbox = gbox_copy(&subbox); */
            if (first)
            {
                gbox_duplicate(&subbox, gbox);
                first = LW_FALSE;
            }
            else
            {
                gbox_merge(&subbox, gbox);
            }
            result = LW_SUCCESS;
        }
    }
    return result;
}

int getPoint4d_p(const POINTARRAY *pa,
                 uint32_t n,
                 POINT4D *op)
{
    uint8_t *ptr;
    int zmflag;

    if (!pa)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "NULL POINTARRAY input");
        return 0;
    }

    if (n >= pa->npoints)
    {
        return 0;
    }

    /* Get a pointer to nth point offset and zmflag */
    ptr = getPoint_internal(pa, n);
    zmflag = FLAGS_GET_ZM(pa->flags);

    switch (zmflag)
    {
        case 0: /* 2d  */
            memcpy(op, ptr, sizeof(POINT2D));
            op->m = NO_M_VALUE;
            op->z = NO_Z_VALUE;
            break;

        case 3: /* ZM */
            memcpy(op, ptr, sizeof(POINT4D));
            break;

        case 2: /* Z */
            memcpy(op, ptr, sizeof(POINT3DZ));
            op->m = NO_M_VALUE;
            break;

        case 1: /* M */
            memcpy(op, ptr, sizeof(POINT3DM));
            op->m = op->z; /* we use Z as temporary storage */
            op->z = NO_Z_VALUE;
            break;

        default:
            CPLError(CE_Failure, CPLE_AppDefined, "Unknown ZM flag ??");
            return 0;
    }
    return 1;
}

double lw_arc_center(const POINT2D *p1,
                     const POINT2D *p2,
                     const POINT2D *p3,
                     POINT2D *result)
{
    POINT2D c;
    double cx, cy, cr;
    double dx21, dy21, dx31, dy31, h21, h31, d;

    c.x = c.y = 0.0;

    /* Closed circle */
    if (fabs(p1->x - p3->x) < EPSILON_SQLMM &&
        fabs(p1->y - p3->y) < EPSILON_SQLMM)
    {
        cx = p1->x + (p2->x - p1->x) / 2.0;
        cy = p1->y + (p2->y - p1->y) / 2.0;
        c.x = cx;
        c.y = cy;
        *result = c;
        cr = sqrt(pow(cx - p1->x, 2.0) + pow(cy - p1->y, 2.0));
        return cr;
    }

    /* Using cartesian eguations from page https://en.wikipedia.org/wiki/Circumscribed_circle */
    dx21 = p2->x - p1->x;
    dy21 = p2->y - p1->y;
    dx31 = p3->x - p1->x;
    dy31 = p3->y - p1->y;

    h21 = pow(dx21, 2.0) + pow(dy21, 2.0);
    h31 = pow(dx31, 2.0) + pow(dy31, 2.0);

    /* 2 * |Cross product|, d<0 means clockwise and d>0 counterclockwise sweeping angle */
    d = 2 * (dx21 * dy31 - dx31 * dy21);

    /* Check colinearity, |Cross product| = 0 */
    if (fabs(d) < EPSILON_SQLMM)
        return -1.0;

    /* Calculate centroid coordinates and radius */
    cx = p1->x + (h21 * dy31 - h31 * dy21) / d;
    cy = p1->y - (h21 * dx31 - h31 * dx21) / d;
    c.x = cx;
    c.y = cy;
    *result = c;
    cr = sqrt(pow(cx - p1->x, 2) + pow(cy - p1->y, 2));

    return cr;
}

int lw_arc_calculate_gbox_cartesian_2d(const POINT2D *A1,
                                       const POINT2D *A2,
                                       const POINT2D *A3,
                                       GBOX *gbox)
{
    POINT2D xmin, ymin, xmax, ymax;
    POINT2D C;
    int A2_side;
    double radius_A;

    radius_A = lw_arc_center(A1, A2, A3, &C);

    /* Negative radius signals straight line, p1/p2/p3 are collinear */
    if (radius_A < 0.0)
    {
        gbox->xmin = FP_MIN(A1->x, A3->x);
        gbox->ymin = FP_MIN(A1->y, A3->y);
        gbox->xmax = FP_MAX(A1->x, A3->x);
        gbox->ymax = FP_MAX(A1->y, A3->y);
        return LW_SUCCESS;
    }

    /* Matched start/end points imply circle */
    if (A1->x == A3->x && A1->y == A3->y)
    {
        gbox->xmin = C.x - radius_A;
        gbox->ymin = C.y - radius_A;
        gbox->xmax = C.x + radius_A;
        gbox->ymax = C.y + radius_A;
        return LW_SUCCESS;
    }

    /* First approximation, bounds of start/end points */
    gbox->xmin = FP_MIN(A1->x, A3->x);
    gbox->ymin = FP_MIN(A1->y, A3->y);
    gbox->xmax = FP_MAX(A1->x, A3->x);
    gbox->ymax = FP_MAX(A1->y, A3->y);

    /* Create points for the possible extrema */
    xmin.x = C.x - radius_A;
    xmin.y = C.y;
    ymin.x = C.x;
    ymin.y = C.y - radius_A;
    xmax.x = C.x + radius_A;
    xmax.y = C.y;
    ymax.x = C.x;
    ymax.y = C.y + radius_A;

    /* Divide the circle into two parts, one on each side of a line
       joining p1 and p3. The circle extrema on the same side of that line
       as p2 is on, are also the extrema of the bbox. */

    A2_side = lw_segment_side(A1, A3, A2);

    if (A2_side == lw_segment_side(A1, A3, &xmin))
        gbox->xmin = xmin.x;

    if (A2_side == lw_segment_side(A1, A3, &ymin))
        gbox->ymin = ymin.y;

    if (A2_side == lw_segment_side(A1, A3, &xmax))
        gbox->xmax = xmax.x;

    if (A2_side == lw_segment_side(A1, A3, &ymax))
        gbox->ymax = ymax.y;

    return LW_SUCCESS;
}

static int lw_arc_calculate_gbox_cartesian(const POINT4D *p1,
                                           const POINT4D *p2,
                                           const POINT4D *p3,
                                           GBOX *gbox)
{
    int rv;
    rv = lw_arc_calculate_gbox_cartesian_2d((POINT2D *)p1, (POINT2D *)p2,
                                            (POINT2D *)p3, gbox);
    gbox->zmin = FP_MIN(p1->z, p3->z);
    gbox->mmin = FP_MIN(p1->m, p3->m);
    gbox->zmax = FP_MAX(p1->z, p3->z);
    gbox->mmax = FP_MAX(p1->m, p3->m);
    return rv;
}

static int lwcircstring_calculate_gbox_cartesian(LWCIRCSTRING *curve,
                                                 GBOX *gbox)
{
    GBOX tmp = {0};
    POINT4D p1, p2, p3;
    uint32_t i;

    if (!curve)
        return LW_FAILURE;
    if (curve->points->npoints < 3)
        return LW_FAILURE;

    tmp.flags =
        ogr_lwflags(FLAGS_GET_Z(curve->flags), FLAGS_GET_M(curve->flags), 0);

    /* Initialize */
    gbox->xmin = gbox->ymin = gbox->zmin = gbox->mmin = FLT_MAX;
    gbox->xmax = gbox->ymax = gbox->zmax = gbox->mmax = -1 * FLT_MAX;

    for (i = 2; i < curve->points->npoints; i += 2)
    {
        getPoint4d_p(curve->points, i - 2, &p1);
        getPoint4d_p(curve->points, i - 1, &p2);
        getPoint4d_p(curve->points, i, &p3);

        if (lw_arc_calculate_gbox_cartesian(&p1, &p2, &p3, &tmp) == LW_FAILURE)
            continue;

        gbox_merge(&tmp, gbox);
    }

    return LW_SUCCESS;
}

int lwgeom_calculate_gbox_cartesian(const LWGEOM *lwgeom,
                                    GBOX *gbox)
{
    if (!lwgeom)
        return LW_FAILURE;

    switch (lwgeom->type)
    {
        case POINTTYPE:
            return lwpoint_calculate_gbox_cartesian((LWPOINT *)lwgeom, gbox);
        case LINETYPE:
            return lwline_calculate_gbox_cartesian((LWLINE *)lwgeom, gbox);
        case CIRCSTRINGTYPE:
            return lwcircstring_calculate_gbox_cartesian((LWCIRCSTRING *)lwgeom,
                                                         gbox);
        case POLYGONTYPE:
            return lwpoly_calculate_gbox_cartesian((LWPOLY *)lwgeom, gbox);
        case TRIANGLETYPE:
            return lwtriangle_calculate_gbox_cartesian((LWTRIANGLE *)lwgeom,
                                                       gbox);
        case COMPOUNDTYPE:
        case CURVEPOLYTYPE:
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTICURVETYPE:
        case MULTIPOLYGONTYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return lwcollection_calculate_gbox_cartesian((LWCOLLECTION *)lwgeom,
                                                         gbox);
    }
    /* Never get here, please. */
    CPLError(CE_Failure, CPLE_AppDefined, "unsupported type (%d)",
             lwgeom->type);
    return LW_FAILURE;
}

int lwgeom_calculate_gbox(const LWGEOM *lwgeom,
                          GBOX *gbox)
{
    gbox->flags = lwgeom->flags;
    if (FLAGS_GET_GEODETIC(lwgeom->flags))
        return lwgeom_calculate_gbox_geodetic(lwgeom, gbox);
    else
        return lwgeom_calculate_gbox_cartesian(lwgeom, gbox);
}

void lwgeom_add_bbox(LWGEOM *lwgeom)
{
    /* an empty LWGEOM has no bbox */
    if (lwgeom_is_empty(lwgeom))
        return;

    if (lwgeom->bbox)
        return;
    FLAGS_SET_BBOX(lwgeom->flags, 1);
    lwgeom->bbox = gbox_new(lwgeom->flags);
    lwgeom_calculate_gbox(lwgeom, lwgeom->bbox);
}

static int lwflags_uses_extended_flags(lwflags_t lwflags)
{
    lwflags_t core_lwflags =
        LWFLAG_Z | LWFLAG_M | LWFLAG_BBOX | LWFLAG_GEODETIC;
    return (lwflags & (~core_lwflags)) != 0;
}

size_t gbox_serialized_size(lwflags_t flags)
{
    if (FLAGS_GET_GEODETIC(flags))
        return 6 * sizeof(float);
    else
        return 2 * FLAGS_NDIMS(flags) * sizeof(float);
}

static size_t
gserialized2_from_any_size(const LWGEOM *geom); /* Local prototype */

static size_t gserialized2_from_lwpoint_size(const LWPOINT *point)
{
    size_t size = 4; /* Type number. */

    size += 4; /* Number of points (one or zero (empty)). */
    size += point->point->npoints * FLAGS_NDIMS(point->flags) * sizeof(double);

    return size;
}

static size_t gserialized2_from_lwline_size(const LWLINE *line)
{
    size_t size = 4; /* Type number. */

    size += 4; /* Number of points (zero => empty). */
    size += line->points->npoints * FLAGS_NDIMS(line->flags) * sizeof(double);

    return size;
}

static size_t gserialized2_from_lwtriangle_size(const LWTRIANGLE *triangle)
{
    size_t size = 4; /* Type number. */

    size += 4; /* Number of points (zero => empty). */
    size += triangle->points->npoints * FLAGS_NDIMS(triangle->flags) *
            sizeof(double);

    return size;
}

static size_t gserialized2_from_lwpoly_size(const LWPOLY *poly)
{
    size_t size = 4; /* Type number. */
    uint32_t i = 0;
    const size_t point_size = FLAGS_NDIMS(poly->flags) * sizeof(double);

    size += 4; /* Number of rings (zero => empty). */
    if (poly->nrings % 2)
        size += 4; /* Padding to double alignment. */

    for (i = 0; i < poly->nrings; i++)
    {
        size += 4; /* Number of points in ring. */
        size += poly->rings[i]->npoints * point_size;
    }

    return size;
}

static size_t gserialized2_from_lwcircstring_size(const LWCIRCSTRING *curve)
{
    size_t size = 4; /* Type number. */

    size += 4; /* Number of points (zero => empty). */
    size += curve->points->npoints * FLAGS_NDIMS(curve->flags) * sizeof(double);

    return size;
}

static size_t gserialized2_from_lwcollection_size(const LWCOLLECTION *col)
{
    size_t size = 4; /* Type number. */
    uint32_t i = 0;

    size += 4; /* Number of sub-geometries (zero => empty). */

    for (i = 0; i < col->ngeoms; i++)
    {
        size_t subsize = gserialized2_from_any_size(col->geoms[i]);
        size += subsize;
    }

    return size;
}

static size_t gserialized2_from_any_size(const LWGEOM *geom)
{
    switch (geom->type)
    {
        case POINTTYPE:
            return gserialized2_from_lwpoint_size((LWPOINT *)geom);
        case LINETYPE:
            return gserialized2_from_lwline_size((LWLINE *)geom);
        case POLYGONTYPE:
            return gserialized2_from_lwpoly_size((LWPOLY *)geom);
        case TRIANGLETYPE:
            return gserialized2_from_lwtriangle_size((LWTRIANGLE *)geom);
        case CIRCSTRINGTYPE:
            return gserialized2_from_lwcircstring_size((LWCIRCSTRING *)geom);
        case CURVEPOLYTYPE:
        case COMPOUNDTYPE:
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTICURVETYPE:
        case MULTIPOLYGONTYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return gserialized2_from_lwcollection_size((LWCOLLECTION *)geom);
        default:
            CPLError(CE_Failure, CPLE_AppDefined, "Unknown geometry type: %d",
                     geom->type);
            return 0;
    }
}

size_t gserialized2_from_lwgeom_size(const LWGEOM *geom)
{
    size_t size = 8; /* Header overhead (varsize+flags+srid) */

    /* Reserve space for extended flags */
    if (lwflags_uses_extended_flags(geom->flags))
        size += 8;

    /* Reserve space for bounding box */
    if (geom->bbox)
        size += gbox_serialized_size(geom->flags);

    size += gserialized2_from_any_size(geom);
    return size;
}

void gserialized2_set_srid(GSERIALIZED *g,
                           int32_t srid)
{
    srid = clamp_srid(srid);

    /* 0 is our internal unknown value.
     * We'll map back and forth here for now */
    if (srid == SRID_UNKNOWN)
        srid = 0;

    g->srid[0] = (srid & 0x001F0000) >> 16;
    g->srid[1] = (srid & 0x0000FF00) >> 8;
    g->srid[2] = (srid & 0x000000FF);
}

uint8_t lwflags_get_g2flags(lwflags_t lwflags)
{
    uint8_t gflags = 0;
    G2FLAGS_SET_Z(gflags, FLAGS_GET_Z(lwflags));
    G2FLAGS_SET_M(gflags, FLAGS_GET_M(lwflags));
    G2FLAGS_SET_BBOX(gflags, FLAGS_GET_BBOX(lwflags));
    G2FLAGS_SET_GEODETIC(gflags, FLAGS_GET_GEODETIC(lwflags));
    G2FLAGS_SET_EXTENDED(gflags, lwflags_uses_extended_flags(lwflags));
    G2FLAGS_SET_VERSION(gflags, 1);
    return gflags;
}

size_t gserialized2_from_extended_flags(lwflags_t lwflags,
                                        uint8_t *buf)
{
    if (lwflags_uses_extended_flags(lwflags))
    {
        uint64_t xflags = 0;
        if (FLAGS_GET_SOLID(lwflags))
            xflags |= G2FLAG_X_SOLID;

        memcpy(buf, &xflags, sizeof(uint64_t));
        return sizeof(uint64_t);
    }
    return 0;
}

inline float next_float_down(double d)
{
    float result;
    if (d > (double)FLT_MAX)
        return FLT_MAX;
    if (d <= (double)-FLT_MAX)
        return -FLT_MAX;
    result = (float)d;

    if (((double)result) <= d)
        return result;

    return nextafterf(result, -1 * FLT_MAX);
}

/*
 * Returns the float that's very close to the input, but >=.
 * handles the funny differences in float4 and float8 reps.
 */
inline float next_float_up(double d)
{
    float result;
    if (d >= (double)FLT_MAX)
        return FLT_MAX;
    if (d < (double)-FLT_MAX)
        return -FLT_MAX;
    result = (float)d;

    if (((double)result) >= d)
        return result;

    return nextafterf(result, FLT_MAX);
}

size_t gserialized2_from_gbox(const GBOX *gbox,
                              uint8_t *buf)
{
    uint8_t *loc = buf;
    float *f;
    uint8_t i = 0;
    size_t return_size;

    f = (float *)buf;
    f[i++] = next_float_down(gbox->xmin);
    f[i++] = next_float_up(gbox->xmax);
    f[i++] = next_float_down(gbox->ymin);
    f[i++] = next_float_up(gbox->ymax);
    loc += 4 * sizeof(float);

    if (FLAGS_GET_GEODETIC(gbox->flags))
    {
        f[i++] = next_float_down(gbox->zmin);
        f[i++] = next_float_up(gbox->zmax);
        loc += 2 * sizeof(float);

        return_size = (size_t)(loc - buf);
        return return_size;
    }

    if (FLAGS_GET_Z(gbox->flags))
    {
        f[i++] = next_float_down(gbox->zmin);
        f[i++] = next_float_up(gbox->zmax);
        loc += 2 * sizeof(float);
    }

    if (FLAGS_GET_M(gbox->flags))
    {
        f[i++] = next_float_down(gbox->mmin);
        f[i++] = next_float_up(gbox->mmax);
        loc += 2 * sizeof(float);
    }
    return_size = (size_t)(loc - buf);
    return return_size;
}

size_t gserialized2_from_lwgeom_any(const LWGEOM *geom,
                                    uint8_t *buf);

static size_t gserialized2_from_lwpoint(const LWPOINT *point,
                                        uint8_t *buf)
{
    uint8_t *loc;
    size_t ptsize = ptarray_point_size(point->point);
    int type = POINTTYPE;

    if (FLAGS_GET_ZM(point->flags) != FLAGS_GET_ZM(point->point->flags))
        CPLError(CE_Failure, CPLE_AppDefined, "Dimensions mismatch in lwpoint");

    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);
    /* Write in the number of points (0 => empty). */
    memcpy(loc, &(point->point->npoints), sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Copy in the ordinates. */
    if (point->point->npoints > 0)
    {
        memcpy(loc, getPoint_internal(point->point, 0), ptsize);
        loc += ptsize;
    }

    return (size_t)(loc - buf);
}

static size_t gserialized2_from_lwline(const LWLINE *line,
                                       uint8_t *buf)
{
    uint8_t *loc;
    size_t ptsize;
    size_t size;
    int type = LINETYPE;

    if (FLAGS_GET_Z(line->flags) != FLAGS_GET_Z(line->points->flags))
        CPLError(CE_Failure, CPLE_AppDefined, "Dimensions mismatch in lwline");

    ptsize = ptarray_point_size(line->points);

    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the npoints. */
    memcpy(loc, &(line->points->npoints), sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Copy in the ordinates. */
    if (line->points->npoints > 0)
    {
        size = line->points->npoints * ptsize;
        memcpy(loc, getPoint_internal(line->points, 0), size);
        loc += size;
    }
    return (size_t)(loc - buf);
}

static size_t gserialized2_from_lwpoly(const LWPOLY *poly,
                                       uint8_t *buf)
{
    uint32_t i;
    uint8_t *loc;
    int ptsize;
    int type = POLYGONTYPE;

    ptsize = sizeof(double) * FLAGS_NDIMS(poly->flags);
    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the nrings. */
    memcpy(loc, &(poly->nrings), sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the npoints per ring. */
    for (i = 0; i < poly->nrings; i++)
    {
        memcpy(loc, &(poly->rings[i]->npoints), sizeof(uint32_t));
        loc += sizeof(uint32_t);
    }

    /* Add in padding if necessary to remain double aligned. */
    if (poly->nrings % 2)
    {
        memset(loc, 0, sizeof(uint32_t));
        loc += sizeof(uint32_t);
    }

    /* Copy in the ordinates. */
    for (i = 0; i < poly->nrings; i++)
    {
        POINTARRAY *pa = poly->rings[i];
        size_t pasize;

        if (FLAGS_GET_ZM(poly->flags) != FLAGS_GET_ZM(pa->flags))
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Dimensions mismatch in lwpoly");

        pasize = pa->npoints * ptsize;
        if (pa->npoints > 0)
            memcpy(loc, getPoint_internal(pa, 0), pasize);
        loc += pasize;
    }
    return (size_t)(loc - buf);
}

static size_t gserialized2_from_lwtriangle(const LWTRIANGLE *triangle,
                                           uint8_t *buf)
{
    uint8_t *loc;
    size_t ptsize;
    size_t size;
    int type = TRIANGLETYPE;

    if (FLAGS_GET_ZM(triangle->flags) != FLAGS_GET_ZM(triangle->points->flags))
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Dimensions mismatch in lwtriangle");

    ptsize = ptarray_point_size(triangle->points);

    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the npoints. */
    memcpy(loc, &(triangle->points->npoints), sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Copy in the ordinates. */
    if (triangle->points->npoints > 0)
    {
        size = triangle->points->npoints * ptsize;
        memcpy(loc, getPoint_internal(triangle->points, 0), size);
        loc += size;
    }
    return (size_t)(loc - buf);
}

static size_t gserialized2_from_lwcircstring(const LWCIRCSTRING *curve,
                                             uint8_t *buf)
{
    uint8_t *loc;
    size_t ptsize;
    size_t size;
    int type = CIRCSTRINGTYPE;

    if (FLAGS_GET_ZM(curve->flags) != FLAGS_GET_ZM(curve->points->flags))
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Dimensions mismatch in lwcircstring");

    ptsize = ptarray_point_size(curve->points);
    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the npoints. */
    memcpy(loc, &curve->points->npoints, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Copy in the ordinates. */
    if (curve->points->npoints > 0)
    {
        size = curve->points->npoints * ptsize;
        memcpy(loc, getPoint_internal(curve->points, 0), size);
        loc += size;
    }

    return (size_t)(loc - buf);
}

static size_t gserialized2_from_lwcollection(const LWCOLLECTION *coll,
                                             uint8_t *buf)
{
    size_t subsize = 0;
    uint8_t *loc;
    uint32_t i;
    int type;

    type = coll->type;
    loc = buf;

    /* Write in the type. */
    memcpy(loc, &type, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Write in the number of subgeoms. */
    memcpy(loc, &coll->ngeoms, sizeof(uint32_t));
    loc += sizeof(uint32_t);

    /* Serialize subgeoms. */
    for (i = 0; i < coll->ngeoms; i++)
    {
        if (FLAGS_GET_ZM(coll->flags) != FLAGS_GET_ZM(coll->geoms[i]->flags))
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Dimensions mismatch in lwcollection");
        subsize = gserialized2_from_lwgeom_any(coll->geoms[i], loc);
        loc += subsize;
    }

    return (size_t)(loc - buf);
}

size_t gserialized2_from_lwgeom_any(const LWGEOM *geom, uint8_t *buf)
{
    switch (geom->type)
    {
        case POINTTYPE:
            return gserialized2_from_lwpoint((LWPOINT *)geom, buf);
        case LINETYPE:
            return gserialized2_from_lwline((LWLINE *)geom, buf);
        case POLYGONTYPE:
            return gserialized2_from_lwpoly((LWPOLY *)geom, buf);
        case TRIANGLETYPE:
            return gserialized2_from_lwtriangle((LWTRIANGLE *)geom, buf);
        case CIRCSTRINGTYPE:
            return gserialized2_from_lwcircstring((LWCIRCSTRING *)geom, buf);
        case CURVEPOLYTYPE:
        case COMPOUNDTYPE:
        case MULTIPOINTTYPE:
        case MULTILINETYPE:
        case MULTICURVETYPE:
        case MULTIPOLYGONTYPE:
        case MULTISURFACETYPE:
        case POLYHEDRALSURFACETYPE:
        case TINTYPE:
        case COLLECTIONTYPE:
            return gserialized2_from_lwcollection((LWCOLLECTION *)geom, buf);
        default:
            CPLError(CE_Failure, CPLE_AppDefined, "Unknown geometry type: %d",
                     geom->type);
            return 0;
    }
}

GSERIALIZED *gserialized_from_lwgeom(LWGEOM *geom,
                                     size_t *size)
{
    size_t expected_size = 0;
    size_t return_size = 0;
    uint8_t *ptr = NULL;
    GSERIALIZED *g = NULL;

    /*
    ** See if we need a bounding box, add one if we don't have one.
    */
    if ((!geom->bbox) && lwgeom_needs_bbox(geom) && (!lwgeom_is_empty(geom)))
    {
        lwgeom_add_bbox(geom);
    }

    /*
    ** Harmonize the flags to the state of the lwgeom
    */
    FLAGS_SET_BBOX(geom->flags, (geom->bbox ? 1 : 0));

    /* Set up the uint8_t buffer into which we are going to write the serialized geometry. */
    expected_size = gserialized2_from_lwgeom_size(geom);
    ptr = (uint8_t *)CPLMalloc(expected_size);
    g = (GSERIALIZED *)(ptr);

    /* Set the SRID! */
    gserialized2_set_srid(g, geom->srid);
    LWSIZE_SET(g->size, expected_size);
    g->gflags = lwflags_get_g2flags(geom->flags);

    /* Move write head past size, srid and flags. */
    ptr += 8;

    /* Write in the extended flags if necessary */
    ptr += gserialized2_from_extended_flags(geom->flags, ptr);

    /* Write in the serialized form of the gbox, if necessary. */
    if (geom->bbox)
        ptr += gserialized2_from_gbox(geom->bbox, ptr);

    /* Write in the serialized form of the geometry. */
    ptr += gserialized2_from_lwgeom_any(geom, ptr);

    /* Calculate size as returned by data processing functions. */
    return_size = ptr - (uint8_t *)g;

    if (size) /* Return the output size to the caller if necessary. */
        *size = return_size;

    return g;
}

GSERIALIZED *gserialized_from_ewkb(const char *hexwkb,
                                   size_t *size)
{
    size_t hexwkb_len;
    unsigned char *wkb;
    LWGEOM *lwgeom;
    GSERIALIZED *result;

    if (!hexwkb)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "lwgeom_from_hexwkb: null input");
        return NULL;
    }

    hexwkb_len = strlen(hexwkb);
    wkb = bytes_from_hexbytes(hexwkb, hexwkb_len);
    lwgeom = lwgeom_from_wkb(wkb, hexwkb_len / 2, LW_PARSER_CHECK_ALL);
    CPLFree(wkb);
    result = gserialized_from_lwgeom(lwgeom, size);
    lwgeom_free(lwgeom);
    return result;
}
