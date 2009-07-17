/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/ProjectedMap>
#include <osgEarth/Mercator>
#include <osgEarth/TerrainTileEdgeNormalizerUpdateCallback>
#include <osgEarth/Compositing>
#include <osgEarth/ImageUtils>
#include <osgEarth/EarthTerrainTechnique>
#include <osgEarth/FileLocationCallback>

#include <osg/Image>
#include <osg/Notify>
#include <osg/PagedLOD>
#include <osg/CoordinateSystemNode>
#include <osg/Version>
#include <osgDB/ReadFile>
#include <osgTerrain/Terrain>
#include <osgTerrain/TerrainTile>
#include <osgTerrain/Locator>
#include <osgTerrain/GeometryTechnique>
#include <sstream>
#include <stdlib.h>

using namespace osgEarth;
using namespace OpenThreads;

ProjectedMap::ProjectedMap()
{
    //NOP
}


osg::Node*
ProjectedMap::createQuadrant( const MapConfig& mapConfig, osgTerrain::Terrain* terrain, const TileKey* key )
{
    ScopedReadLock lock( mapConfig.getSourceMutex() );
    double xmin, ymin, xmax, ymax;
    key->getGeoExtent().getBounds(xmin, ymin, xmax, ymax);

    bool empty_map = mapConfig.getImageSources().size() == 0 && mapConfig.getHeightFieldSources().size() == 0;

    GeoImageList image_tiles;

    //TODO: select/composite:
    if ( mapConfig.getImageSources().size() > 0 )
    {
        //Add an image from each image source
        for (unsigned int i = 0; i < mapConfig.getImageSources().size(); ++i)
        {
            TileSource* source = mapConfig.getImageSources()[i].get();
            GeoImage* image = NULL;
            if (source->isKeyValid(key))
            {
                image = createGeoImage(key, source);
            }
            else
            {
                //If the image is not valid, create an empty texture as a placeholder
                image = new GeoImage(ImageUtils::getEmptyImage(), key->getGeoExtent());
            }
            image_tiles.push_back(image);
        }
    }


    bool hasElevation = false;
    //Create the heightfield for the tile
    osg::ref_ptr<osg::HeightField> hf = NULL;
    if ( mapConfig.getHeightFieldSources().size() > 0 )
    {
        hf = createHeightField(mapConfig, key, false);
        hasElevation = hf.valid();
    }

    //Determine if we've created any images
    unsigned int numValidImages = 0;
    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].valid()) numValidImages++;
    }

    // If we couldn't create any imagery of heightfields, bail out
    if (!hf.valid() && (numValidImages == 0))
    {
        osg::notify(osg::INFO) << "[osgEarth::ProjectedMap] Could not create any imagery or heightfields for " << key->str() <<".  Not building tile" << std::endl;
        return NULL;
    }
   
    //Try to interpolate any missing image layers from parent tiles
    for (unsigned int i = 0; i < mapConfig.getImageSources().size(); ++i)
    {
        if (!image_tiles[i].valid())
        {
            TileSource* source = mapConfig.getImageSources()[i].get();
            if (source->isKeyValid(key))
            {
                GeoImage* image = createValidGeoImage(source, key);
                if (image)
                {
                    osg::notify(osg::INFO) << "[osgEarth::ProjectedMap] Using fallback image for image source " << source->getName() << " for TileKey " << key->str() << std::endl;
                    image_tiles[i] = image;
                }
                else
                {
                    osg::notify(osg::INFO) << "[osgEarth::ProjectedMap] Could not get valid image from image source " << source->getName() << " for TileKey " << key->str() << std::endl;
                }
            }
        }
    }

    //Fill in missing heightfield information from parent tiles
    if (!hf.valid())
    {
        //We have no heightfield sources, 
        if (mapConfig.getHeightFieldSources().size() == 0)
        {
            hf = createEmptyHeightField( key );
        }
        else
        {
            //Try to get a heightfield again, but this time fallback on parent tiles
            hf = createHeightField( mapConfig, key, true );
            if (!hf.valid())
            {
                osg::notify(osg::WARN) << "[osgEarth::ProjectedMap] Could not get valid heightfield for TileKey " << key->str() << std::endl;
                return NULL;
            }
            else
            {
                hasElevation = true;
            }
        }
    }

    //Scale the heightfield elevations from meters to degrees
    if ( mapConfig.getProfile()->getSRS()->isGeographic() )
    {
        scaleHeightFieldToDegrees( hf.get() );
    }

    osgTerrain::Locator* geo_locator = mapConfig.getProfile()->getSRS()->createLocator(
        xmin, ymin, xmax, ymax,
        mapConfig.getProfile()->getSRS()->isGeographic() ); 

    osgTerrain::HeightFieldLayer* hf_layer = new osgTerrain::HeightFieldLayer();
    hf_layer->setLocator( geo_locator );
    hf_layer->setHeightField( hf.get() );

    osgTerrain::TerrainTile* tile = new osgTerrain::TerrainTile();
    tile->setLocator( geo_locator );
    //tile->setTerrainTechnique( new osgTerrain::GeometryTechnique() );
    tile->setTerrainTechnique( new osgEarth::EarthTerrainTechnique() );
    tile->setElevationLayer( hf_layer );
    tile->setRequiresNormals( true );
    tile->setDataVariance(osg::Object::DYNAMIC);
    tile->setTileID(key->getTileId());

    if (hasElevation && mapConfig.getNormalizeEdges())
    {
        //Attach an updatecallback to normalize the edges of TerrainTiles.
        tile->setUpdateCallback(new TerrainTileEdgeNormalizerUpdateCallback());
        tile->setDataVariance(osg::Object::DYNAMIC);
    }

    //Assign the terrain system to the TerrainTile.
    //It is very important the terrain system is set while the MapConfig's sourceMutex is locked.
    //This registers the terrain tile so that adding/removing layers are always in sync.  If you don't do this
    //you can end up with a situation where the database pager is waiting to merge a tile, then a layer is added, then
    //the tile is finally merged and is out of sync.
    tile->setTerrain( terrain );

    int layer = 0;
    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].valid())
        {
            double img_xmin, img_ymin, img_xmax, img_ymax;

            //Specify a new locator for the color with the coordinates of the TileKey that was actually used to create the image
            osg::ref_ptr<osgTerrain::Locator> img_locator;

            GeoImage* geo_image = image_tiles[i].get();

            //Special case for when the map is geographic and the image is Mercator
            if ( mapConfig.getProfile()->getSRS()->isGeographic() && geo_image->getSRS()->isMercator() )
            {
                //Transform the mercator extents to geographic
                GeoExtent geog_ext = image_tiles[i]->getExtent().transform( image_tiles[i]->getExtent().getSRS()->getGeographicSRS() );
                geog_ext.getBounds( img_xmin, img_ymin, img_xmax, img_ymax );
                img_locator = mapConfig.getProfile()->getSRS()->createLocator( img_xmin, img_ymin, img_xmax, img_ymax );
                img_locator = new MercatorLocator( *img_locator.get(), geo_image->getExtent() );
            }
            else
            {
                image_tiles[i]->getExtent().getBounds( img_xmin, img_ymin, img_xmax, img_ymax );

                img_locator = mapConfig.getProfile()->getSRS()->createLocator(
                    img_xmin, img_ymin, img_xmax, img_ymax,
                    mapConfig.getProfile()->getSRS()->isGeographic() );
            }

            osgTerrain::ImageLayer* img_layer = new osgTerrain::ImageLayer( geo_image->getImage() );
            //osgTerrain::ImageLayer* img_layer = new osgEarth::osgEarthImageLayer(imageLayers[i]->getId(), geo_image->getImage() );
            img_layer->setLocator( img_locator.get() );

            tile->setColorLayer( layer, img_layer );
            layer++;
        }
    }
    
    osg::Vec3d centroid( (xmax+xmin)/2.0, (ymax+ymin)/2.0, 0 );

    double max_range = 1e10;
    double radius = (centroid-osg::Vec3d(xmin,ymin,0)).length();
    double min_range = radius * mapConfig.getMinTileRangeFactor();

    //Set the skirt height of the heightfield
    hf->setSkirtHeight(radius * mapConfig.getSkirtRatio());

    // see if we need to keep subdividing:
    osg::PagedLOD* plod = new osg::PagedLOD();
    plod->setCenter( centroid );
    plod->addChild( tile, min_range, max_range );
    plod->setFileName( 1, createURI( mapConfig.getId(), key ) );
    plod->setRange( 1, 0.0, min_range );

#if USE_FILELOCATIONCALLBACK
    osgDB::Options* options = new osgDB::Options;
    options->setFileLocationCallback( new osgEarth::FileLocationCallback);
    plod->setDatabaseOptions( options );
#endif

    return plod;
}

void
ProjectedMap::scaleHeightFieldToDegrees(osg::HeightField *hf)
{
    //The number of degrees in a meter at the equator
    //TODO: adjust this calculation based on the actual EllipsoidModel.
    float scale = 1.0f/111319.0f;
    if (hf)
    {
        for (unsigned int i = 0; i < hf->getHeightList().size(); ++i)
        {
            hf->getHeightList()[i] *= scale;
        }
    }
    else
    {
        osg::notify(osg::WARN) << "[osgEarth::ProjectedMap] scaleHeightFieldToDegrees heightfield is NULL" << std::endl;
    }
}
