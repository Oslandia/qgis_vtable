/***************************************************************************
           qgsvirtuallayerprovider.cpp Virtual layer data provider
begin                : Jan, 2014
copyright            : (C) 2014 Hugo Mercier, Oslandia
email                : hugo dot mercier at oslandia dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

extern "C" {
#include <sqlite3.h>
#include <spatialite.h>
}

#include <QUrl>

#include "qgsvirtuallayerprovider.h"
#include <qgsvectorlayer.h>
#include <qgsmaplayerregistry.h>
#include <qgsdatasourceuri.h>

const QString VIRTUAL_LAYER_KEY = "virtual";
const QString VIRTUAL_LAYER_DESCRIPTION = "Virtual layer data provider";

// declaration of the spatialite module
extern "C" {
    int qgsvlayer_module_init();
}

#define PROVIDER_ERROR( msg ) do { mError = QgsError( msg, VIRTUAL_LAYER_KEY ); QgsDebugMsg( msg ); } while(0)

QgsFields getSqliteFields( sqlite3* db, const QString& table )
{
    QgsFields fields;

    sqlite3_stmt *stmt;
    int r;
    r = sqlite3_prepare_v2( db, QString("PRAGMA table_info('%1')").arg(table).toUtf8().constData(), -1, &stmt, NULL );
    if (r) {
        throw std::runtime_error( sqlite3_errmsg( db ) );
    }
    int n = sqlite3_column_count( stmt );
    while ( (r=sqlite3_step( stmt )) == SQLITE_ROW ) {
        QString field_name((const char*)sqlite3_column_text( stmt, 1 ));
        QString field_type((const char*)sqlite3_column_text( stmt, 2 ));
        std::cout << field_name.toUtf8().constData() << " - " << field_type.toUtf8().constData() << std::endl;

        if ( field_type == "INT" ) {
            fields.append( QgsField( field_name, QVariant::Int ) );
        }
        else if ( field_type == "REAL" ) {
            fields.append( QgsField( field_name, QVariant::Double ) );
        }
        else if ( field_type == "TEXT" ) {
            fields.append( QgsField( field_name, QVariant::String ) );
        }
        else if (( field_type == "POINT" ) || (field_type == "MULTIPOINT") ||
                 (field_type == "LINESTRING") || (field_type == "MULTILINESTRING") ||
                 (field_type == "POLYGON") || (field_type == "MULTIPOLYGON")) {
            continue;
        }
        else {
            // force to TEXT
            fields.append( QgsField( field_name, QVariant::String ) );
        }
    }

    sqlite3_finalize( stmt );
    return fields;
}

QgsVirtualLayerProvider::QgsVirtualLayerProvider( QString const &uri )
    : QgsVectorDataProvider( uri ),
      mValid( true ),
      mSpatialite( 0 )
{
    QUrl url = QUrl::fromEncoded( uri.toUtf8() );
    if ( !url.isValid() ) {
        mValid = false;
        PROVIDER_ERROR("Malformed URL");
        return;
    }

    // xxxxx = open a virtual layer
    // xxxxx?key=value&key=value = create a virtual layer
    // ?key=value = create a temporary virtual layer

    mPath = url.path();

    // geometry field set by parameters
    GeometryField geometryField;

    QList<QPair<QString, QString> > items = url.queryItems();
    for ( int i = 0; i < items.size(); i++ ) {
        QString key = items.at(i).first;
        QString value = items.at(i).second;
        if ( key == "layer_id" ) {
            QgsMapLayer *l = QgsMapLayerRegistry::instance()->mapLayer( value );
            if ( l == 0 ) {
                mValid = false;
                PROVIDER_ERROR( QString("Cannot find layer %1").arg(value) );
                return;
            }
            if ( l->type() != QgsMapLayer::VectorLayer ) {
                mValid = false;
                PROVIDER_ERROR( QString("Layer %1 is not a vector layer").arg(value) );
                return;
            }
            // add the layer to the list
            mLayers << static_cast<QgsVectorLayer*>(l);
        }
        else if ( key == "geometry" ) {
            // geometry field definition, optional
            // geometry_column:wkb_type:srid
            QRegExp reGeom( "(\\w+):(\\d+):(\\d+)" );
            int pos = reGeom.indexIn( value );
            if ( pos >= 0 ) {
                geometryField.name = reGeom.cap(1);
                geometryField.wkbType = reGeom.cap(2).toLong();
                geometryField.srid = reGeom.cap(3).toLong();
                mGeometryFields << geometryField;
            }
        }
        else if ( key == "uid" ) {
            mUid = value;
        }
        else if ( key == "query" ) {
            mQuery = value;
        }
    }

    // consistency check
    if ( mLayers.size() > 1 && mQuery.isEmpty() ) {
        mValid = false;
        PROVIDER_ERROR( QString("Don't know how to join layers, please specify a query") );
        return;
    }

    if ( !mQuery.isEmpty() && mUid.isEmpty() ) {
        mValid = false;
        PROVIDER_ERROR( QString("Please specify a 'uid' column name") );
        return;
    }

    if ( mLayers.size() > 1 && geometryField.name.isEmpty() ) {
        mValid = false;
        PROVIDER_ERROR( QString("Please specify the geometry column name and type") );
        return;
    }

    spatialite_init(0);

    // use a temporary file if needed
    if ( mPath.isEmpty() ) {
        mTempFile.reset( new QTemporaryFile() );
        mTempFile->open();
        mPath = mTempFile->fileName();
        mTempFile->close();
    }

    sqlite3* db;
    // open and create if it does not exist
    int r = sqlite3_open( mPath.toUtf8().constData(), &db );
    if ( r ) {
        mValid = false;
        PROVIDER_ERROR( QString( sqlite3_errmsg(db) ) );
        return;
    }
    mSqlite.reset(db);

    // now create virtual tables based on layers
    int layer_idx = 0;
    bool has_geometry = false;
    for ( int i = 0; i < mLayers.size(); i++ ) {
        layer_idx++;
        QgsVectorLayer* vlayer = mLayers.at(i);

        QString geometry_type_str;
        int geometry_dim;
        int geometry_wkb_type;
        {
            switch ( vlayer->dataProvider()->geometryType() ) {
            case QGis::WKBNoGeometry:
                geometry_type_str = "";
                geometry_dim = 0;
                geometry_wkb_type = 0;
                break;
            case QGis::WKBPoint:
                geometry_type_str = "POINT";
                geometry_dim = 2;
                geometry_wkb_type = 1;
                break;
            case QGis::WKBPoint25D:
                geometry_type_str = "POINT";
                geometry_dim = 3;
                geometry_wkb_type = 1001;
                break;
            case QGis::WKBMultiPoint:
                geometry_type_str = "MULTIPOINT";
                geometry_dim = 2;
                geometry_wkb_type = 4;
                break;
            case QGis::WKBMultiPoint25D:
                geometry_type_str = "MULTIPOINT";
                geometry_dim = 3;
                geometry_wkb_type = 1004;
                break;
            case QGis::WKBLineString:
                geometry_type_str = "LINESTRING";
                geometry_dim = 2;
                geometry_wkb_type = 2;
                break;
            case QGis::WKBLineString25D:
                geometry_type_str = "LINESTRING";
                geometry_dim = 3;
                geometry_wkb_type = 1002;
                break;
            case QGis::WKBMultiLineString:
                geometry_type_str = "MULTILINESTRING";
                geometry_dim = 2;
                geometry_wkb_type = 5;
                break;
            case QGis::WKBMultiLineString25D:
                geometry_type_str = "MULTILINESTRING";
                geometry_dim = 3;
                geometry_wkb_type = 1005;
                break;
            case QGis::WKBPolygon:
                geometry_type_str = "POLYGON";
                geometry_dim = 2;
                geometry_wkb_type = 3;
                break;
            case QGis::WKBPolygon25D:
                geometry_type_str = "POLYGON";
                geometry_dim = 3;
                geometry_wkb_type = 1003;
                break;
            case QGis::WKBMultiPolygon:
                geometry_type_str = "MULTIPOLYGON";
                geometry_dim = 2;
                geometry_wkb_type = 6;
                break;
            case QGis::WKBMultiPolygon25D:
                geometry_type_str = "MULTIPOLYGON";
                geometry_dim = 3;
                geometry_wkb_type = 1006;
                break;
            }
        }
        long srid = vlayer->crs().postgisSrid();

        QString createStr = QString("SELECT InitSpatialMetadata(1); DROP TABLE IF EXISTS vtab%1; CREATE VIRTUAL TABLE vtab%1 USING QgsVLayer(%2);").arg(layer_idx).arg(vlayer->id());
        if ( geometry_wkb_type ) {
            has_geometry = true;
            createStr += QString( "INSERT OR REPLACE INTO virts_geometry_columns (virt_name, virt_geometry, geometry_type, coord_dimension, srid) "
                                  "VALUES ('vtab%1', 'geometry', %2, %3, %4 );" ).arg(layer_idx).arg(geometry_wkb_type).arg(geometry_dim).arg(srid);

            // manually set column statistics (needed for QGIS spatialite provider)
            QgsRectangle extent = vlayer->extent();
            createStr += QString("INSERT OR REPLACE INTO virts_geometry_columns_statistics (virt_name, virt_geometry, last_verified, row_count, extent_min_x, extent_min_y, extent_max_x, extent_max_y) "
                                 "VALUES ('vtab%1', 'geometry', datetime('now'), %2, %3, %4, %5, %6);")
                .arg(layer_idx)
                .arg(vlayer->featureCount())
                .arg(extent.xMinimum())
                .arg(extent.yMinimum())
                .arg(extent.xMaximum())
                .arg(extent.yMaximum());
        }

        char *errMsg;
        int r = sqlite3_exec( mSqlite.data(), createStr.toUtf8().constData(), NULL, NULL, &errMsg );
        if (r) {
            mValid = false;
            PROVIDER_ERROR( errMsg );
            return;
        }
    }

    if ( !mQuery.isEmpty() ) {
        // create a temporary view, in order to call table_info on it
        QString viewStr = "CREATE TEMPORARY VIEW vview AS " + mQuery;

        char *errMsg;
        int r;
        r = sqlite3_exec( mSqlite.data(), viewStr.toUtf8().constData(), NULL, NULL, &errMsg );
        if (r) {
            mValid = false;
            PROVIDER_ERROR( errMsg );
            return;
        }

        // look for column names and types
        try {
            mFields = getSqliteFields( mSqlite.data(), "vview" );
        }
        catch ( std::runtime_error& e ) {
            mValid = false;
            PROVIDER_ERROR( e.what() );
            return;
        }

        QgsDataSourceURI source;
        source.setDatabase( mPath );
        source.setDataSource( "", "(" + mQuery + ")", mGeometryFields.isEmpty() ? "" : mGeometryFields[0].name, "", mUid );
        std::cout << "Spatialite uri: " << source.uri().toUtf8().constData() << std::endl;
        mSpatialite = new QgsSpatiaLiteProvider( source.uri() );
        
    }
    else {
        // no query => implies we must only have one virtual table
        QgsDataSourceURI source;
        source.setDatabase( mPath );
        source.setDataSource( "", "vtab1", has_geometry ? "geometry" : "" );
        std::cout << "Spatialite uri: " << source.uri().toUtf8().constData() << std::endl;
        mSpatialite = new QgsSpatiaLiteProvider( source.uri() );

        mFields = mSpatialite->fields();
        GeometryField g_field;
        g_field.name = "geometry";
        // FIXME
        //        g_field.type = ..
        mGeometryFields << g_field;
    }

    mValid = mSpatialite->isValid();
}

QgsVirtualLayerProvider::~QgsVirtualLayerProvider()
{
    if (mSpatialite) {
        delete mSpatialite;
    }
}

QgsAbstractFeatureSource* QgsVirtualLayerProvider::featureSource() const
{
    return mSpatialite->featureSource();
}

QString QgsVirtualLayerProvider::storageType() const
{
    return "No storage per se, view data from other data sources";
}

QgsCoordinateReferenceSystem QgsVirtualLayerProvider::crs()
{
    return mSpatialite->crs();
}

QgsFeatureIterator QgsVirtualLayerProvider::getFeatures( const QgsFeatureRequest& request )
{
    return mSpatialite->getFeatures( request );
}

QString QgsVirtualLayerProvider::subsetString()
{
    return "";
}

bool QgsVirtualLayerProvider::setSubsetString( QString theSQL, bool updateFeatureCount )
{
}


QGis::WkbType QgsVirtualLayerProvider::geometryType() const
{
    return mSpatialite->geometryType();
}

size_t QgsVirtualLayerProvider::layerCount() const
{
    return mLayers.size();
}

long QgsVirtualLayerProvider::featureCount() const
{
    return mSpatialite->featureCount();
}

QgsRectangle QgsVirtualLayerProvider::extent()
{
    return mSpatialite->extent();
}

void QgsVirtualLayerProvider::updateExtents()
{
    mSpatialite->updateExtents();
}

const QgsFields & QgsVirtualLayerProvider::fields() const
{
    return mFields;
}

QVariant QgsVirtualLayerProvider::minimumValue( int index )
{
    return mSpatialite->minimumValue( index );
}

QVariant QgsVirtualLayerProvider::maximumValue( int index )
{
    return mSpatialite->maximumValue( index );
}

void QgsVirtualLayerProvider::uniqueValues( int index, QList < QVariant > &uniqueValues, int limit )
{
    return mSpatialite->uniqueValues( index, uniqueValues, limit );
}

bool QgsVirtualLayerProvider::isValid()
{
    return mValid;
}

int QgsVirtualLayerProvider::capabilities() const
{
    return 0; //SelectAtId | SelectGeometryAtId;
}

QString QgsVirtualLayerProvider::name() const
{
    return VIRTUAL_LAYER_KEY;
}

QString QgsVirtualLayerProvider::description() const
{
    return VIRTUAL_LAYER_DESCRIPTION;
}

QgsAttributeList QgsVirtualLayerProvider::pkAttributeIndexes()
{
    return mSpatialite->pkAttributeIndexes();
}

/**
 * Class factory to return a pointer to a newly created
 * QgsSpatiaLiteProvider object
 */
QGISEXTERN QgsVirtualLayerProvider *classFactory( const QString * uri )
{
    // register sqlite extension
    sqlite3_auto_extension( (void(*)())qgsvlayer_module_init );

    return new QgsVirtualLayerProvider( *uri );
}

/** Required key function (used to map the plugin to a data store type)
*/
QGISEXTERN QString providerKey()
{
  return VIRTUAL_LAYER_KEY;
}

/**
 * Required description function
 */
QGISEXTERN QString description()
{
  return VIRTUAL_LAYER_DESCRIPTION;
}

/**
 * Required isProvider function. Used to determine if this shared library
 * is a data provider plugin
 */
QGISEXTERN bool isProvider()
{
  return true;
}

QGISEXTERN void cleanupProvider()
{
    // unregister sqlite extension
    sqlite3_cancel_auto_extension( (void(*)())qgsvlayer_module_init );
}
