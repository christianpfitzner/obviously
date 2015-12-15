#include "TSD_PDFMatching.h"

#define STRUCTAPPROACH 0

namespace obvious
{

TSD_PDFMatching::TSD_PDFMatching(TsdGrid& grid, unsigned int trials, double epsThresh, unsigned int sizeControlSet, double zhit, double zphi, double zshort, double zmax, double zrand, double percentagePointsInC, double rangemax, double sigphi, double sighit, double lamshort, double maxAngleDiff, double maxAnglePenalty) :
    RandomMatching(sizeControlSet), _grid(grid)
{

  _scaleDistance = 1.0 / (epsThresh * epsThresh);
  _scaleOrientation = 0.33;

  _trace = NULL;
  _pcaSearchRange = 10;

  _trials = trials;
  _sizeControlSet = sizeControlSet;

  _percentagePointsInC = percentagePointsInC;  // how many percent of control set have to be in field of view

  // probability parameters vgl. Book: Probabistic Robotics
  _zhit = zhit;
  _zmax = zmax;
  _zshort = zshort;
  _zrand = zrand;
  _zphi = zphi;  // additional parameter for phi error; not used at the moment

  _rangemax = rangemax;
  _sighit = sighit;
  _sigphi = sigphi;  // additional parameter for phi error; not used at the moment
  _lamshort = lamshort;

  _maxAngleDiff = maxAngleDiff;
  _maxAnglePenalty = maxAnglePenalty;
}

TSD_PDFMatching::~TSD_PDFMatching()
{

}

obvious::Matrix TSD_PDFMatching::match(
    const obvious::Matrix sensorTrans,
    const obvious::Matrix* M,
    const bool* maskM,
    const obvious::Matrix* NM,
    const obvious::Matrix* S,
    const bool* maskS,
    double phiMax,
    const double transMax,
    const double resolution)
{
  double probOfActualScan;
  obfloat tsd;
  obfloat coord[2];
  obvious::Matrix pointInSensor(3, 1);
  obvious::Matrix TBest(3, 3);
  TBest.setIdentity();

  const int pointsInM = M->getRows();
  const int pointsInS = S->getRows();

  if(pointsInM != pointsInS)
  {
    LOGMSG(DBG_ERROR, "Model and scene need to be of same size, size of M: " << pointsInM << ", size of S: " << pointsInS);
    return TBest;
  }

  if(pointsInM < 3)
  {
    LOGMSG(DBG_ERROR, "Model and scene contain too less points, size of M: " << pointsInM << ", size of S: " << pointsInS);
    return TBest;
  }

  // ----------------- Model ------------------
  obvious::Matrix* NMpca = new Matrix(pointsInM, 2);  // Normals for model
  double* phiM = new double[pointsInM];    // Orientation of model points
  bool* maskMpca = new bool[pointsInM];      // Validity mask of model points

  memcpy(maskMpca, maskM, pointsInM * sizeof(bool));

  if(NM)
  {
    calcPhi(NM, maskM, phiM);
  }
  else  // if normals are not supplied
  {
    calcNormals(M, NMpca, maskM, maskMpca, _pcaSearchRange/2);
    calcPhi(NMpca, maskMpca, phiM);
  }
  vector<unsigned int> idxMValid = extractSamples(M, maskMpca, _pcaSearchRange / 2);

  // -------------------------------------------

  // ----------------- Scene -------------------
  obvious::Matrix* NSpca = new Matrix(pointsInS, 2);  // Normals for scene
  double* phiS = new double[pointsInS];    // Orientation of scene points
  bool* maskSpca = new bool[pointsInS];      // Validity mask of scene points
  memcpy(maskSpca, maskS, pointsInS * sizeof(bool));

  // Determine number of valid samples in local scene neighborhood
  // only from these points a valid orientation is computable
  unsigned int validPoints = 0;
  for(int i = 0; i < pointsInS; i++)
    if(maskSpca[i])
      validPoints++;

  // Probability of point masking
  double probability = 180.0 / (double)validPoints;
  if(probability < 0.99)
    subsampleMask(maskSpca, pointsInS, probability);

  calcNormals(S, NSpca, maskS, maskSpca, _pcaSearchRange/2);
  calcPhi(NSpca, maskSpca, phiS);

  vector<unsigned int> idxSValid = extractSamples(S, maskSpca, _pcaSearchRange / 2);
  // -------------------------------------------

  // --------------- Control set ---------------
  vector<unsigned int> idxControl;  //represents the indices of points used for Control in S.
  obvious::Matrix* Control = pickControlSet(S, idxSValid, idxControl);
  obvious::Matrix* NControl = new obvious::Matrix(idxControl.size(), 2);
  for(unsigned int i = 0; i < Control->getCols(); i++)
  {
    (*NControl)(i, 0) = (*NSpca)(idxControl[i], 0);
    (*NControl)(i, 1) = (*NSpca)(idxControl[i], 1);
  }
  unsigned int pointsInC = Control->getCols();
  //unsigned int cntMatchThresh = pointsInC * _percentagePointsInC;  // TODO: Determine meaningful parameter
  double* phiControl = new double[pointsInC];  // Orientation of control points
  calcPhi(NControl, NULL, phiControl);
  // -------------------------------------------//

  // Determine frustum, i.e., direction of leftmost and rightmost model point
  //double thetaMin = -((double)pointsInM - 1.0) / 2.0 * resolution;  // theoretical bounding
  double thetaBoundMin = atan2((*M)(idxMValid.front(), 1), (*M)(idxMValid.front(), 0));  // real bounding
  double thetaBoundMax = atan2((*M)(idxMValid.back(), 1), (*M)(idxMValid.back(), 0));  // real bounding

  LOGMSG(DBG_DEBUG, "Valid points in scene: " << idxSValid.size() << ", valid points in model: " << idxMValid.size() << ", Control set: " << Control->getCols());
  LOGMSG(DBG_DEBUG, "Model phi min:: " << rad2deg(thetaBoundMin) << ", Model phi max: " << rad2deg(thetaBoundMax));

  if(idxSValid.size() < 3)
  {
    LOGMSG(DBG_ERROR, "Too less valid points in scene, matchable size: " << idxSValid.size());
    return TBest;
  }

  if(idxMValid.size() < 3)
  {
    LOGMSG(DBG_ERROR, "Too less valid points in model, matchable size: " << idxMValid.size());
    return TBest;
  }

  // Check for maximum meaningful trials
  unsigned int trials = _trials;
  if(idxMValid.size() < _trials)
    trials = idxMValid.size();

  if(_trace)
  {
    _trace->reset();
    _trace->setModel(M, idxMValid);
    _trace->setScene(S, idxSValid);
  }

  // Calculate search "radius", i.e., maximum difference in polar indices because of rotation
  phiMax = min(phiMax, M_PI * 0.5);
  int span;
  if(resolution > 1e-6)
  {
    span = floor(phiMax / resolution);
    if(span > (int)pointsInM)
      span = (int)pointsInM;
  }
  else
  {
    LOGMSG(DBG_ERROR, "Resolution not properly set: resolution = " << resolution);
    return TBest;
  }

  srand(time(NULL));

  //double bestRatio = 0.0;
  double bestProb = 0.0;

#ifndef DEBUG
  // trace is only possible for single threaded execution
  if(_trace)
  {
    omp_set_num_threads(1);
    LOGMSG(DBG_WARN, "Configured single-threaded execution due to application of trace module");
  }
#endif

  //Timer t;
  //t.start();
  vector<unsigned int> idxTrials = idxMValid;

  bool* maskControl = new bool[pointsInC];

//#pragma omp parallel for
  for(unsigned int trial = 0; trial < trials; trial++)
  {

    int idx;
#pragma omp critical
    {
      const int randIdx = rand() % (idxTrials.size());
      idx = idxTrials[randIdx];

      // remove chosen element to avoid picking same index a second time
      idxTrials.erase(idxTrials.begin() + randIdx);
    }

    // leftmost scene point
    const int iMin = max(idx - span, _pcaSearchRange / 2);
    // rightmost scene point
    const int iMax = min(idx + span, pointsInS - _pcaSearchRange / 2);

    for(int i = iMin; i < iMax; i++)
    {

#if STRUCTAPPROACH
      if(samplesS[i].valid)
#else
      if(maskSpca[i])
#endif
      {

#if STRUCTAPPROACH
        double phi = samplesM[idx].orientation - samplesS[i].orientation;
#else
        double phi = phiM[idx] - phiS[i];
#endif
        if(phi > M_PI)
          phi -= 2.0 * M_PI;
        else if(phi < -M_PI)
          phi += 2.0 * M_PI;

        if(fabs(phi) < phiMax)
        {
          obvious::Matrix T = obvious::MatrixFactory::TransformationMatrix33(phi, 0, 0);

          // Calculate translation
          // todo: dan: trying to use phi + dx, dy directly for transformation via rightsided multiplication!?
          const double sx = (*S)(i, 0);
          const double sy = (*S)(i, 1);
          T(0, 2) = (*M)(idx, 0) - (T(0, 0) * sx + T(0, 1) * sy);
          T(1, 2) = (*M)(idx, 1) - (T(1, 0) * sx + T(1, 1) * sy);

          // Transform control set
          obvious::Matrix STemp = T * (*Control);
          unsigned int pointsInControl = STemp.getCols();

          // Rating Daniel Ammon & Tobias Fink
          std::vector<double> probOfAllScans;  // vector for probabilities of single scans in one measurement
          double probOfActualMeasurement = 1;

          for (unsigned int s = 0; s < pointsInControl; s++)	// whole control set
          {
            pointInSensor(0, 0) = (double)((STemp)(0, s));
            pointInSensor(1, 0) = (double)((STemp)(1, s));
            pointInSensor(2, 0) = 1.0;

            obvious::Matrix pointInMap(sensorTrans * pointInSensor); // transform from sensor in map system

            coord[0] = pointInMap(0, 0);
            coord[1] = pointInMap(1, 0);

            if( !_grid.interpolateBilinear(coord, &tsd) )
            {
              probOfActualScan = (double)tsd;
            }
            else
            {
              probOfActualScan = 1.0;
            }

            probOfActualScan = 1.0-fabs(probOfActualScan);	// rating function

            // todo: magic numbers 0.05 + 0.95
            probOfActualScan = 0.05 + 0.95*probOfActualScan;  // random probability --> avoid prob of 0

            probOfActualMeasurement *= probOfActualScan; // multiply all probabilities for probability of whole scan

          }  // whole control set

          // update T and bestProb if better than last iteration
          if(probOfActualMeasurement > bestProb)
          {
            TBest = T;
            bestProb = probOfActualMeasurement;

            if(_trace)
            {
              //trace is only possible for single threaded execution
              vector<unsigned int> idxM;
              idxM.push_back(idx);
              vector<unsigned int> idxS;
              idxS.push_back(i);
              _trace->addAssignment(M, idxM, S, idxS, &STemp, 10 * probOfActualMeasurement, trial);
            }
          }
        } // if cntMatch
      }  // STRUCTAPPROACH ???
    }  // for i
  }  // for trials

  delete[] maskControl;

  //cout << "elapsed: " << t.elapsed() << endl;
  //t.reset();

  delete NMpca;
  delete NSpca;
  delete[] phiM;
  delete[] phiS;
  delete[] phiControl;
  delete[] maskMpca;
  delete[] maskSpca;

  delete Control;

  return TBest;
}


void TSD_PDFMatching::analyzeTSD(double p1[2], double p2[2], double window, double resolution)
{
  obvious::Matrix v(1,2);
  obvious::Matrix pa(1,2);
  double lengthV;

  pa(0,0) = p1[0];
  pa(0,1) = p1[1];

  v(0,0) = p2[0] - p1[0];
  v(0,1) = p2[1] - p1[1];

  lengthV = sqrt(pow(v(0,0), 2) + pow(v(0,1), 2));

  // normalize v
  v(0,0) = v(0,0)/lengthV;
  v(0,1) = v(0,1)/lengthV;

  double increment;
  increment = 2.0 / resolution;

  for(double x = -1.0; x <= +1.0; x += increment)
  {
    obvious::Matrix pos(1,2);
    obvious::Matrix pb(1,2);
    obvious::Matrix pc(1,2);

    pb(0,0) = v(0,0) * lengthV;
    pb(0,1) = v(0,1) * lengthV;

    pc(0,0) = v(0,0) * window * x;
    pc(0,1) = v(0,1) * window * x;

    pos(0,0) = pa(0,0) + pb(0,0) + pc(0,0);
    pos(0,1) = pa(0,1) + pb(0,1) + pc(0,1);


    obfloat coord[2];
    coord[0] = pos(0,0);
    coord[1] = pos(0,1);

    obfloat tsd;
    _grid.interpolateBilinear(coord, &tsd);
    cout << ";" << coord[0] << "|" << coord[1] << "|" << tsd << endl;
  }

  cout << ";" << p2[0] << ";" << p2[1] << endl;

  cout << ";" << "+++" << endl;

}

} /* namespace obvious */
