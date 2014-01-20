#include "TsdGridComponent.h"
#include "obcore/math/mathbase.h"
#include <cmath>

namespace obvious
{

TsdGridComponent::TsdGridComponent(bool isLeaf)
{
  _isLeaf = isLeaf;
}

TsdGridComponent::~TsdGridComponent()
{

}

double TsdGridComponent::getComponentSize()
{
  return _componentSize;
}

double* TsdGridComponent::getCentroid()
{
  return _centroid;
}

double TsdGridComponent::getCircumradius()
{
  return _circumradius;
}

Matrix* TsdGridComponent::getEdgeCoordsHom()
{
  return _edgeCoordsHom;
}

bool TsdGridComponent::isLeaf()
{
  return _isLeaf;
}

bool TsdGridComponent::isInRange(double pos[2], Sensor* sensor, double maxTruncation)
{
  // Centroid-to-sensor distance
  double distance = euklideanDistance<double>(pos, _centroid, 2);


  double maxRange = sensor->getMaximumRange();

  // closest possible distance of any voxel in partition
  double minDist = distance - _circumradius - maxTruncation;

  // check if partition is out of range
  if(minDist > maxRange) return false;


  double minRange = sensor->getMinimumRange();

  // farthest possible distance of any voxel in partition
  double maxDist = distance + _circumradius + maxTruncation;

  // check if partition is too close
  if(maxDist < minRange) return false;

  double* data = sensor->getRealMeasurementData();
  bool* mask = sensor->getRealMeasurementMask();


  int idxEdge[4];
  sensor->backProject(_edgeCoordsHom, idxEdge);

  int minIdx;
  int maxIdx;
  minmaxArray<int>(idxEdge, 4, &minIdx, &maxIdx);

  // Check whether non of the cells are in the field of view
  if(maxIdx<0) return false;

  if(minIdx<0) minIdx = 0;


  // Check if any cell comes closer than the truncation radius
  bool isVisible = false;
  bool isEmpty = true;
  for(int j=minIdx; j<maxIdx; j++)
  {
    if(mask[j])
    {
      isVisible = isVisible || (data[j] > minDist);
      if(isVisible)
      {
        isEmpty = isEmpty && (data[j] > maxDist);
      }
    }
  }

  if(!isVisible) return false;

  if(isEmpty)
  {
    increaseEmptiness();
    return false;
  }

  return true;
}

}
