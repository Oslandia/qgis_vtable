#include <qgsvirtuallayerfeatureiterator.h>
#include "vlayer_module.h"

static QString quotedColumn( QString name )
{
    return "\"" + name.replace("\"", "\"\"") + "\"";
}

QgsVirtualLayerFeatureIterator::QgsVirtualLayerFeatureIterator( QgsVirtualLayerFeatureSource* source, bool ownSource, const QgsFeatureRequest& request )
    : QgsAbstractFeatureIteratorFromSource<QgsVirtualLayerFeatureSource>( source, ownSource, request )
{
    mPath = mSource->provider()->mPath;
    mSqlite = Sqlite::open( mPath );
    mDefinition = mSource->provider()->mDefinition;

    mSqlQuery = mDefinition.query();

    QStringList wheres;
    QString subset = mSource->provider()->mSubset;
    if ( !subset.isNull() ) {
        wheres << subset;
    }

    if ( !mDefinition.geometryField().isNull() && request.filterType() == QgsFeatureRequest::FilterRect ) {
        bool do_exact = request.flags() & QgsFeatureRequest::ExactIntersect;
        QgsRectangle rect( request.filterRect() );
        QString mbr = QString("%1,%2,%3,%4").arg(rect.xMinimum()).arg(rect.yMinimum()).arg(rect.xMaximum()).arg(rect.yMaximum());
        wheres <<  QString("%1Intersects(%2,BuildMbr(%3))")
            .arg(do_exact ? "Mbr" : "")
            .arg(quotedColumn(mDefinition.geometryField()))
            .arg(mbr);
    }
    else if (!mDefinition.uid().isNull() && request.filterType() == QgsFeatureRequest::FilterFid ) {
        wheres << QString("%1=%2")
            .arg(quotedColumn(mDefinition.uid()))
            .arg(request.filterFid());
    }
    else if (!mDefinition.uid().isNull() && request.filterType() == QgsFeatureRequest::FilterFids ) {
        QString values = quotedColumn(mDefinition.uid()) + " IN (";
        bool first = true;
        for ( auto& v : request.filterFids() ) {
            if (!first) {
                values += ",";
            }
            first = false;
            values += QString::number(v);
        }
        values += ")";
        wheres << values;
    }

    if ( request.flags() & QgsFeatureRequest::SubsetOfAttributes ) {
        // copy only selected fields
        for ( int idx: request.subsetOfAttributes() ) {
            mFields.append( mSource->provider()->fields().at(idx) );
        }
    }
    else {
        mFields = mSource->provider()->fields();
    }

    QString columns;
    {
        // the first column is always the uid (or 0)
        if ( !mDefinition.uid().isNull() ) {
            columns = quotedColumn(mDefinition.uid());
        }
        else {
            columns = "0";
        }
        for ( int i = 0; i < mFields.count(); i++ ) {
            columns += ",";
            QString cname = mFields.at(i).name().toLower();
            columns += quotedColumn(cname);
        }
    }
    // the last column is the geometry, if any
    if ( !(request.flags() & QgsFeatureRequest::NoGeometry) && !mDefinition.geometryField().isNull() ) {
        columns += "," + quotedColumn(mDefinition.geometryField());
    }

    mSqlQuery = "SELECT " + columns + " FROM (" + mSqlQuery + ")";
    if ( !wheres.isEmpty() ) {
        mSqlQuery += " WHERE " + wheres.join(" AND ");
    }

    std::cout << mSqlQuery.toUtf8().constData() << std::endl;

    mQuery.reset( new Sqlite::Query( mSqlite.get(), mSqlQuery ) );

    mFid = 0;
}

QgsVirtualLayerFeatureIterator::~QgsVirtualLayerFeatureIterator()
{
    close();
}

bool QgsVirtualLayerFeatureIterator::rewind()
{
    if (mClosed) {
        return false;
    }

    mQuery->reset();

    return true;
}

bool QgsVirtualLayerFeatureIterator::close()
{
    if (mClosed) {
        return false;
    }

    // this call is absolutely needed
    iteratorClosed();

    mClosed = true;
    return true;
}

bool QgsVirtualLayerFeatureIterator::fetchFeature( QgsFeature& feature )
{
    if (mClosed) {
        return false;
    }
    if (mQuery->step() != SQLITE_ROW) {
        return false;
    }

    feature.setFields( &mFields, /* init */ true );

    if ( mDefinition.uid().isNull() ) {
        // no id column => autoincrement
        feature.setFeatureId( mFid++ );
    }
    else {
        // first column: uid
        feature.setFeatureId( mQuery->column_int64( 0 ) );
    }

    int n = mQuery->column_count();
    for ( int i = 0; i < mFields.count(); i++ ) {
        const QgsField& f = mFields.at(i);
        if ( f.type() == QVariant::Int ) {
            feature.setAttribute( i, mQuery->column_int(i+1) );
        }
        else if ( f.type() == QVariant::Double ) {
            feature.setAttribute( i, mQuery->column_double(i+1) );
        }
        else if ( f.type() == QVariant::String ) {
            feature.setAttribute( i, mQuery->column_text(i+1) );
        }
    }
    if (n > mFields.count()+1) {
        // geometry field
        QByteArray blob( mQuery->column_blob(n-1) );
        std::unique_ptr<QgsGeometry> geom( spatialite_blob_to_qgsgeometry( (const unsigned char*)blob.constData(), blob.size() ) );
        feature.setGeometry( geom.release() );
    }

    return true;
}

QgsVirtualLayerFeatureSource::QgsVirtualLayerFeatureSource( const QgsVirtualLayerProvider* p ) :
    mProvider(p)
{
}

QgsVirtualLayerFeatureSource::~QgsVirtualLayerFeatureSource()
{
}

QgsFeatureIterator QgsVirtualLayerFeatureSource::getFeatures( const QgsFeatureRequest& request )
{
  return QgsFeatureIterator( new QgsVirtualLayerFeatureIterator( this, false, request ) );
}
