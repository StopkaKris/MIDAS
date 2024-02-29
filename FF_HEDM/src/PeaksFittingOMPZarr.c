//
// Copyright (c) 2014, UChicago Argonne, LLC
// See LICENSE file.
//

//
//  PeaksFittingOMPZarr.c
//
//
//  Created by Hemant Sharma on 2024/02/27.
//
//

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <nlopt.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <omp.h>
#include <sys/resource.h>
#include <blosc2.h>
#include <stdlib.h> 
#include <zip.h> 

#define deg2rad 0.0174532925199433
#define rad2deg 57.2957795130823
#define MAXNHKLS 5000
#define MAX_N_RINGS 500
#define nOverlapsMaxPerImage 10000
#define CalcNorm3(x,y,z) sqrt((x)*(x) + (y)*(y) + (z)*(z))
#define CalcNorm2(x,y) sqrt((x)*(x) + (y)*(y))
typedef uint16_t pixelvalue;
double zDiffThresh;

long double diff(struct timespec start, struct timespec end)
{
	long double diff_sec = end.tv_sec - start.tv_sec;
	long double diff_nsec = end.tv_nsec - start.tv_nsec;
	return (diff_sec * 1e6) + (diff_nsec / 1000.0);
}

static inline
pixelvalue**
allocMatrixPX(int nrows, int ncols)
{
    pixelvalue** arr;
    int i;
    arr = malloc(nrows * sizeof(*arr));
    if (arr == NULL ) {
        return NULL;
    }
    for ( i = 0 ; i < nrows ; i++) {
        arr[i] = malloc(ncols * sizeof(*arr[i]));
        if (arr[i] == NULL ) {
            return NULL;
        }
    }
    return arr;
}

static inline
void
FreeMemMatrixPx(pixelvalue **mat,int nrows)
{
    int r;
    for ( r = 0 ; r < nrows ; r++) {
        free(mat[r]);
    }
    free(mat);
}

static inline
double CalcEtaAngle(double y, double z){
	double alpha = rad2deg*acos(z/sqrt(y*y+z*z));
	if (y>0) alpha = -alpha;
	return alpha;
}

static inline
void YZ4mREta(int NrElements, double *R, double *Eta, double *Y, double *Z){
	int i;
	for (i=0;i<NrElements;i++){
		Y[i] = -R[i]*sin(Eta[i]*deg2rad);
		Z[i] = R[i]*cos(Eta[i]*deg2rad);
	}
}

static inline
int**
allocMatrixInt(int nrows, int ncols)
{
    int** arr;
    int i;
    arr = malloc(nrows * sizeof(*arr));
    if (arr == NULL ) {
        return NULL;
    }
    for ( i = 0 ; i < nrows ; i++) {
        arr[i] = malloc(ncols * sizeof(*arr[i]));
        if (arr[i] == NULL ) {
            return NULL;
        }
    }
    return arr;
}

static inline
void
FreeMemMatrixInt(int **mat,int nrows)
{
    int r;
    for ( r = 0 ; r < nrows ; r++) {
        free(mat[r]);
    }
    free(mat);
}

static inline
double**
allocMatrix(int nrows, int ncols)
{
    double** arr;
    int i;
    arr = malloc(nrows * sizeof(*arr));
    if (arr == NULL ) {
        return NULL;
    }
    for ( i = 0 ; i < nrows ; i++) {
        arr[i] = malloc(ncols * sizeof(*arr[i]));
        if (arr[i] == NULL ) {
            return NULL;
        }
    }
    return arr;
}

static inline
void
FreeMemMatrix(double **mat,int nrows)
{
    int r;
    for ( r = 0 ; r < nrows ; r++) {
        free(mat[r]);
    }
    free(mat);
}

static inline double sind(double x){return sin(deg2rad*x);}
static inline double cosd(double x){return cos(deg2rad*x);}
static inline double tand(double x){return tan(deg2rad*x);}
static inline double asind(double x){return rad2deg*(asin(x));}
static inline double acosd(double x){return rad2deg*(acos(x));}
static inline double atand(double x){return rad2deg*(atan(x));}

static inline void Transposer (double *x, int n, double *y)
{
	int i,j;
	for (i=0;i<n;i++){
		for (j=0;j<n;j++){
			y[(i*n)+j] = x[(j*n)+i];
		}
	}
}

const int dx[] = {+1,  0, -1,  0, +1, -1, +1, -1};
const int dy[] = { 0, +1,  0, -1, +1, +1, -1, -1};

static inline void DepthFirstSearch(int x, int y, int current_label, int NrPixels, int *BoolImage, int *ConnectedComponents,int *Positions, int *PositionTrackers)
{
	if (x < 0 || x == NrPixels) return;
	if (y < 0 || y == NrPixels) return;
	if ((ConnectedComponents[x*NrPixels+y]!=0)||(BoolImage[x*NrPixels+y]==0)) return;

	ConnectedComponents[x*NrPixels+y] = current_label;
	Positions[current_label*NrPixels*4+PositionTrackers[current_label]] = (x*NrPixels) + y;
	PositionTrackers[current_label] += 1;
	int direction;
	for (direction=0;direction<8;++direction){
		DepthFirstSearch(x + dx[direction], y + dy[direction], current_label, NrPixels, BoolImage, ConnectedComponents,Positions,PositionTrackers);
	}
}

static inline int FindConnectedComponents(int *BoolImage, int NrPixels, int *ConnectedComponents, int *Positions, int *PositionTrackers){
	int i,j;
	for (i=0;i<NrPixels*NrPixels;i++){
		ConnectedComponents[i] = 0;
	}
	int component = 0;
	for (i=0;i<NrPixels;++i) {
		for (j=0;j<NrPixels;++j) {
			if ((ConnectedComponents[i*NrPixels+j]==0) && (BoolImage[i*NrPixels+j] == 1)){
				DepthFirstSearch(i,j,++component,NrPixels,BoolImage,ConnectedComponents,Positions,PositionTrackers);
			}
		}
	}
	return component;
}

static inline unsigned FindRegionalMaxima(double *z,int *PixelPositions,
		int NrPixelsThisRegion,int *MaximaPositions,double *MaximaValues,
		int *IsSaturated, double IntSat,int NrPixels)
{
	unsigned nPeaks = 0;
	int i,j,k,l;
	double zThis, zMatch;
	int xThis, yThis;
	int xNext, yNext;
	int isRegionalMax = 1;
	for (i=0;i<NrPixelsThisRegion;i++){
		isRegionalMax = 1;
		zThis = z[i];
		if (zThis > IntSat) {
			*IsSaturated = 1;
            return 1;
		} else {
			*IsSaturated = 0;
		}
		xThis = PixelPositions[i*2+0];
		yThis = PixelPositions[i*2+1];
		for (j=0;j<8;j++){
			xNext = xThis + dx[j];
			yNext = yThis + dy[j];
			for (k=0;k<NrPixelsThisRegion;k++){
				if (xNext == PixelPositions[k*2+0] && yNext == PixelPositions[k*2+1] && z[k] > (zThis)){
					isRegionalMax = 0;
				}
			}
		}
		if (isRegionalMax == 1){
			MaximaPositions[nPeaks*2+0] = xThis;
			MaximaPositions[nPeaks*2+1] = yThis;
			MaximaValues[nPeaks] = zThis;
			nPeaks++;
		}
	}
	if (nPeaks==0){
        MaximaPositions[nPeaks*2+0] = PixelPositions[NrPixelsThisRegion+0];
        MaximaPositions[nPeaks*2+1] = PixelPositions[NrPixelsThisRegion+1];
        MaximaValues[nPeaks] = z[NrPixelsThisRegion/2];
        nPeaks=1;
	}
	return nPeaks;
}

struct func_data{
	int NrPixels;
	double *z;
	double *Rs;
	double *Etas;
};

static
double problem_function(
	unsigned n,
	const double *x,
	double *grad,
	void* f_data_trial)
{
	struct func_data *f_data = (struct func_data *) f_data_trial;
	int NrPixels = f_data->NrPixels;
	double *z,*Rs,*Etas;
	z = &(f_data->z[0]);
	Rs = &(f_data->Rs[0]);
	Etas = &(f_data->Etas[0]);
	int nPeaks, i,j,k;
	nPeaks = (n-1)/8;
	double BG = x[0];
	double IMAX[nPeaks], R[nPeaks], Eta[nPeaks], Mu[nPeaks], SigmaGR[nPeaks], SigmaLR[nPeaks], SigmaGEta[nPeaks],SigmaLEta[nPeaks];
	for (i=0;i<nPeaks;i++){
		IMAX[i] = x[(8*i)+1];
		R[i] = x[(8*i)+2];
		Eta[i] = x[(8*i)+3];
		Mu[i] = x[(8*i)+4];
		SigmaGR[i] = x[(8*i)+5];
		SigmaLR[i] = x[(8*i)+6];
		SigmaGEta[i] = x[(8*i)+7];
		SigmaLEta[i] = x[(8*i)+8];
	}
	double TotalDifferenceIntensity = 0, CalcIntensity, IntPeaks;
	double L, G,DR,DE,R2,E2;
	for (i=0;i<NrPixels;i++){
		IntPeaks = 0;
		for (j=0;j<nPeaks;j++){
			DR = Rs[i]-R[j];
			R2 = DR*DR;
			DE = Etas[i]-Eta[j];
			E2 = DE*DE;
			L = 1/(((R2/((SigmaLR[j])*(SigmaLR[j])))+1)*((E2/((SigmaLEta[j])*(SigmaLEta[j])))+1));
			G = exp(-(0.5*(R2/(SigmaGR[j]*SigmaGR[j])))-(0.5*(E2/(SigmaGEta[j]*SigmaGEta[j]))));
			IntPeaks += IMAX[j]*((Mu[j]*L) + ((1-Mu[j])*G));
		}
		CalcIntensity = BG + IntPeaks;
		TotalDifferenceIntensity += (CalcIntensity - z[i])*(CalcIntensity - z[i]);
	}
	return TotalDifferenceIntensity;
}

static inline void CalcIntegratedIntensity(int nPeaks,double *x,double *Rs,double *Etas,int NrPixelsThisRegion,double *IntegratedIntensity,int *NrOfPixels){
	double BG = x[0];
	int i,j;
	double IMAX[nPeaks], R[nPeaks], Eta[nPeaks], Mu[nPeaks], SigmaGR[nPeaks], SigmaLR[nPeaks], SigmaGEta[nPeaks],SigmaLEta[nPeaks];
	for (i=0;i<nPeaks;i++){
		IMAX[i] = x[(8*i)+1];
		R[i] = x[(8*i)+2];
		Eta[i] = x[(8*i)+3];
		Mu[i] = x[(8*i)+4];
		SigmaGR[i] = x[(8*i)+5];
		SigmaLR[i] = x[(8*i)+6];
		SigmaGEta[i] = x[(8*i)+7];
		SigmaLEta[i] = x[(8*i)+8];
	}
	double IntPeaks, L, G, BGToAdd,DR,DE,R2,E2;
	for (j=0;j<nPeaks;j++){
		NrOfPixels[j] = 0;
		IntegratedIntensity[j] = 0;
		for (i=0;i<NrPixelsThisRegion;i++){
			DR = Rs[i]-R[j];
			R2 = DR*DR;
			DE = Etas[i]-Eta[j];
			E2 = DE*DE;
			L = 1/(((R2/((SigmaLR[j])*(SigmaLR[j])))+1)*((E2/((SigmaLEta[j])*(SigmaLEta[j])))+1));
			G = exp(-(0.5*(R2/(SigmaGR[j]*SigmaGR[j])))-(0.5*(E2/(SigmaGEta[j]*SigmaGEta[j]))));
			IntPeaks = IMAX[j]*((Mu[j]*L) + ((1-Mu[j])*G));
			if (IntPeaks > BG){
				NrOfPixels[j] += 1;
				BGToAdd = BG;
			}else{
				BGToAdd = 0;
			}
			IntegratedIntensity[j] += (BGToAdd + IntPeaks);
		}
	}
}

int Fit2DPeaks(unsigned nPeaks, int NrPixelsThisRegion, double *z, int *UsefulPixels, double *MaximaValues,
				int *MaximaPositions, double *IntegratedIntensity, double *IMAX, double *YCEN, double *ZCEN,
				double *RCens, double *EtaCens,double Ycen, double Zcen, double Thresh, int *NrPx,double *OtherInfo,int NrPixels)
{
	unsigned n = 1 + (8*nPeaks);
	double x[n],xl[n],xu[n];
	x[0] = Thresh/2;
	xl[0] = 0;
	xu[0] = Thresh;
	int i,j;
	double *Rs, *Etas;
	Rs = malloc(NrPixelsThisRegion*2*sizeof(*Rs));
	Etas = malloc(NrPixelsThisRegion*2*sizeof(*Etas));
	double RMin=1e8, RMax=0, EtaMin=190, EtaMax=-190;
	for (i=0;i<NrPixelsThisRegion;i++){
		Rs[i] = CalcNorm2(UsefulPixels[i*2+0]-Ycen,UsefulPixels[i*2+1]-Zcen);
		Etas[i] = CalcEtaAngle(-UsefulPixels[i*2+0]+Ycen,UsefulPixels[i*2+1]-Zcen);
		if (Rs[i] > RMax) RMax = Rs[i];
		if (Rs[i] < RMin) RMin = Rs[i];
		if (Etas[i] > EtaMax) EtaMax = Etas[i];
		if (Etas[i] < EtaMin) EtaMin = Etas[i];
	}
	double MaxEtaWidth, MaxRWidth;
	MaxRWidth = (RMax - RMin)/2 + 1;
	MaxEtaWidth = (EtaMax - EtaMin)/2 + atand(2/(RMax+RMin));
	if (EtaMax - EtaMin > 180) MaxEtaWidth -= 180;
	double Width = sqrt(NrPixelsThisRegion/nPeaks);
	if (Width > MaxRWidth) Width = MaxRWidth;
	double initSigmaEta;
	for (i=0;i<nPeaks;i++){
		x[(8*i)+1] = MaximaValues[i]; // Imax
		x[(8*i)+2] = CalcNorm2(MaximaPositions[i*2+0]-Ycen,MaximaPositions[i*2+1]-Zcen); //Radius
		x[(8*i)+3] = CalcEtaAngle(-MaximaPositions[i*2+0]+Ycen,MaximaPositions[i*2+1]-Zcen); // Eta
		x[(8*i)+4] = 0.5; // Mu
		x[(8*i)+5] = Width; //SigmaGR
		x[(8*i)+6] = Width; //SigmaLR
		initSigmaEta = Width/x[(8*i)+2];
		if (atand(initSigmaEta) > MaxEtaWidth) initSigmaEta = tand(MaxEtaWidth)-0.0001;
		x[(8*i)+7] = atand(initSigmaEta); //SigmaGEta //0.5;
		x[(8*i)+8] = atand(initSigmaEta); //SigmaLEta //0.5;

		double dEta = rad2deg*atan(1/x[(8*i)+2]);
		xl[(8*i)+1] = MaximaValues[i]/2;
		xl[(8*i)+2] = x[(8*i)+2] - 1;
		xl[(8*i)+3] = x[(8*i)+3] - dEta;
		xl[(8*i)+4] = 0;
		xl[(8*i)+5] = 0.01;
		xl[(8*i)+6] = 0.01;
		xl[(8*i)+7] = 0.005;
		xl[(8*i)+8] = 0.005;

		xu[(8*i)+1] = MaximaValues[i]*2;
		xu[(8*i)+2] = x[(8*i)+2] + 1;
		xu[(8*i)+3] = x[(8*i)+3] + dEta;
		xu[(8*i)+4] = 1;
		xu[(8*i)+5] = 2*MaxRWidth;
		xu[(8*i)+6] = 2*MaxRWidth;
		xu[(8*i)+7] = 2*MaxEtaWidth;
		xu[(8*i)+8] = 2*MaxEtaWidth;

		//~ for (j=0;j<9;j++) printf("Args: %lf %lf %lf\n",x[8*i+j],xl[8*i+j],xu[8*i+j]);
	}
	struct func_data f_data;
	f_data.NrPixels = NrPixelsThisRegion;
	f_data.Rs = &Rs[0];
	f_data.Etas = &Etas[0];
	f_data.z = &z[0];
	struct func_data *f_datat;
	f_datat = &f_data;
	void *trp = (struct func_data *)  f_datat;
	nlopt_opt opt;
	opt = nlopt_create(NLOPT_LN_NELDERMEAD, n);
	nlopt_set_lower_bounds(opt, xl);
	nlopt_set_upper_bounds(opt, xu);
	nlopt_set_maxtime(opt, 300);
	nlopt_set_min_objective(opt, problem_function, trp);
	double minf;
	int rc = nlopt_optimize(opt, x, &minf);
	nlopt_destroy(opt);
	for (i=0;i<nPeaks;i++){
		IMAX[i] = x[(8*i)+1];
		RCens[i] = x[(8*i)+2];
		EtaCens[i] = x[(8*i)+3];
		if (x[(8*i)+5] > x[(8*i)+6]){
			OtherInfo[2*i] = x[(8*i)+5];
		}else{
			OtherInfo[2*i] = x[(8*i)+6];
		}
		if (x[(8*i)+7] > x[(8*i)+8]){
			OtherInfo[2*i+1] = x[(8*i)+7];
		}else{
			OtherInfo[2*i+1] = x[(8*i)+8];
		}
	}
	YZ4mREta(nPeaks,RCens,EtaCens,YCEN,ZCEN);
	CalcIntegratedIntensity(nPeaks,x,Rs,Etas,NrPixelsThisRegion,IntegratedIntensity,NrPx);
	free(Rs);
	free(Etas);
	return rc;
}

static inline int CheckDirectoryCreation(char Folder[1024])
{
	int e;
    struct stat sb;
	char totOutDir[1024];
	sprintf(totOutDir,"%s/",Folder);
    e = stat(totOutDir,&sb);
    if (e!=0 && errno == ENOENT){
		printf("Output directory did not exist, creating %s\n",totOutDir);
		e = mkdir(totOutDir,S_IRWXU);
		if (e !=0) {printf("Could not make the directory. Exiting\n");return 0;}
	}
	return 1;
}

static inline void DoImageTransformations (int NrTransOpt, int TransOpt[10], pixelvalue *Image, int NrPixels)
{
	int i,j,k,l,m;
    pixelvalue **ImageTemp1, **ImageTemp2;
    ImageTemp1 = allocMatrixPX(NrPixels,NrPixels);
    ImageTemp2 = allocMatrixPX(NrPixels,NrPixels);
	for (k=0;k<NrPixels;k++) for (l=0;l<NrPixels;l++) ImageTemp1[k][l] = Image[(NrPixels*k)+l];
	for (k=0;k<NrTransOpt;k++) {
		if (TransOpt[k] == 1){
			for (l=0;l<NrPixels;l++) for (m=0;m<NrPixels;m++) ImageTemp2[l][m] = ImageTemp1[l][NrPixels-m-1]; //Inverting Y.
		} else if (TransOpt[k] == 2){
			for (l=0;l<NrPixels;l++) for (m=0;m<NrPixels;m++) ImageTemp2[l][m] = ImageTemp1[NrPixels-l-1][m]; //Inverting Z.
		} else if (TransOpt[k] == 3){
			for (l=0;l<NrPixels;l++) for (m=0;m<NrPixels;m++) ImageTemp2[l][m] = ImageTemp1[m][l];
		} else if (TransOpt[k] == 0){
			for (l=0;l<NrPixels;l++) for (m=0;m<NrPixels;m++) ImageTemp2[l][m] = ImageTemp1[l][m];
		}
		for (l=0;l<NrPixels;l++) for (m=0;m<NrPixels;m++) ImageTemp1[l][m] = ImageTemp2[l][m];
	}
	for (k=0;k<NrPixels;k++) for (l=0;l<NrPixels;l++) Image[(NrPixels*k)+l] = ImageTemp2[k][l];
	FreeMemMatrixPx(ImageTemp1,NrPixels);
	FreeMemMatrixPx(ImageTemp2,NrPixels);
}

static inline void MakeSquare (int NrPixels, int NrPixelsY, int NrPixelsZ, pixelvalue *InImage, pixelvalue *OutImage)
{
	int i,j,k;
	if (NrPixelsY == NrPixelsZ){
		memcpy(OutImage,InImage,NrPixels*NrPixels*sizeof(*InImage));
	} else {
		if (NrPixelsY > NrPixelsZ){ // Filling along the slow direction // easy
			memcpy(OutImage,InImage,NrPixelsY*NrPixelsZ*sizeof(*InImage));
		} else {
			for (i=0;i<NrPixelsZ;i++){
				memcpy(OutImage+i*NrPixelsZ,InImage+i*NrPixelsY,NrPixelsY*sizeof(*InImage));
			}
		}
	}
}


static inline
void
MatrixMult(
           double m[3][3],
           double  v[3],
           double r[3])
{
    int i;
    for (i=0; i<3; i++) {
        r[i] = m[i][0]*v[0] +
        m[i][1]*v[1] +
        m[i][2]*v[2];
    }
}

static inline
void
MatrixMultF33(
    double m[3][3],
    double n[3][3],
    double res[3][3])
{
    int r;
    for (r=0; r<3; r++) {
        res[r][0] = m[r][0]*n[0][0] + m[r][1]*n[1][0] + m[r][2]*n[2][0];
        res[r][1] = m[r][0]*n[0][1] + m[r][1]*n[1][1] + m[r][2]*n[2][1];
        res[r][2] = m[r][0]*n[0][2] + m[r][1]*n[1][2] + m[r][2]*n[2][2];
    }
}

static void
check (int test, const char * message, ...)
{
    if (test) {
        va_list args;
        va_start (args, message);
        vfprintf (stderr, message, args);
        va_end (args);
        fprintf (stderr, "\n");
        exit (EXIT_FAILURE);
    }
}

void main(int argc, char *argv[]){
    double start_time = omp_get_wtime();
	if (argc != 5){
		printf("Usage: %s DataFile blockNr nBlocks numProcs\n",argv[0]);
		return;
	}
	char *DataFN = argv[1];
	int blockNr = atoi(argv[2]);
	int nBlocks = atoi(argv[3]);
	int numProcs = atoi(argv[4]);

    // open DataFN and get the info needed.
    blosc2_init();
    // Read zarr config
    int errorp = 0;
    zip_t* arch = NULL;
    arch = zip_open(DataFN,0,&errorp);
    if (errorp!=NULL) return 1;
    struct zip_stat* finfo = NULL;
    finfo = calloc(16384, sizeof(int));
    zip_stat_init(finfo);
    zip_file_t* fd = NULL;
    int count = 0;
    char* s = NULL;
    char* arr = NULL;
    int nFrames,NrPixelsZ,NrPixelsY,bytesPerPx,darkLoc=-1,dataLoc=-1,floodLoc=-1;
    int nDarks=0, nFloods=0;
    int locImTransOpt,locRingThresh,nRingsThresh=0,locOmegaRanges,nOmegaRanges=0;
    double omegaStart, omegaStep;
    int32_t dsize;
    char* data = NULL;
	double bc=1, Ycen=1024, Zcen=1024, IntSat=14000, Lsd=1000000, px=200, Width=1000;
	double RhoD=204800, tx=0, ty=0, tz=0, p0=0, p1=0, p2=0, p3=0,Wavelength=0.189714;
	int NrPixels=2048,LayerNr=1,nImTransOpt=0,DoFullImage=0;
	int minNrPx=1, maxNrPx=10000, makeMap = 0, maxNPeaks=400, skipFrame = 0;
	char *TmpFolder;
	TmpFolder = "Temp";
	zDiffThresh = 0;
	double BadPxIntensity = 0;
    char *resultFolder=NULL, dummy[2048];
    while ((zip_stat_index(arch, count, 0, finfo)) == 0) {
        if (strstr(finfo->name,"exchange/data/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                if (3 == sscanf(ptr3, "%*[^0123456789]%d%*[^0123456789]%d%*[^0123456789]%d", &nFrames, &NrPixelsZ, &NrPixelsY)){
                            printf("nFrames: %d nrPixelsZ: %d nrPixelsY: %d\n", nFrames, NrPixelsZ, NrPixelsY);
                        } else return 1;
            } else return 1;
            ptr = strstr(s,"dtype");
            if (ptr!=NULL){
                char *ptrt = strstr(ptr,":");
                char *ptr2 = strstr(ptrt,",");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt+3,loc-4);
                if (strncmp(ptr3,"<u2",3)==0) bytesPerPx = 2;
                if (strncmp(ptr3,"<u4",3)==0) bytesPerPx = 4;
            } else return 1;
            free(s);
        }
        if (strstr(finfo->name,"exchange/dark/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                if (3 == sscanf(ptr3, "%*[^0123456789]%d%*[^0123456789]%d%*[^0123456789]%d", &nDarks, &NrPixelsZ, &NrPixelsY)){
                            printf("nDarks: %d nrPixelsZ: %d nrPixelsY: %d\n", nDarks, NrPixelsZ, NrPixelsY);
                        } else return 1;
            } else return 1;
            free(s);
        }
        if (strstr(finfo->name,"exchange/flood/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                if (3 == sscanf(ptr3, "%*[^0123456789]%d%*[^0123456789]%d%*[^0123456789]%d", &nFloods, &NrPixelsZ, &NrPixelsY)){
                            printf("nFloods: %d nrPixelsZ: %d nrPixelsY: %d\n", nFloods, NrPixelsZ, NrPixelsY);
                        } else return 1;
            } else return 1;
            free(s);
        }
        if (strstr(finfo->name,"exchange/data/0.0.0")!=NULL){
            dataLoc = count;
        }
        if (strstr(finfo->name,"exchange/dark/0.0.0")!=NULL){
            darkLoc = count;
        }
        if (strstr(finfo->name,"exchange/flood/0.0.0")!=NULL){
            floodLoc = count;
        }
        if (strstr(finfo->name,"measurement/process/scan_parameters/start/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            omegaStart = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"measurement/process/scan_parameters/step/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            omegaStep = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ResultFolder/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = 4096;
            resultFolder = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,resultFolder,dsize);
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/MaxNPeaks/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            maxNPeaks = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/SkipFrame/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            skipFrame = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/zDiffThresh/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            zDiffThresh = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/tx/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            tx = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ty/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            ty = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/tz/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            tz = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/p0/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            p0 = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/p1/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            p1 = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/p2/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            p2 = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/p3/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            p3 = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/MinNrPx/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            minNrPx = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/MaxNrPx/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            maxNrPx = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/DoFullImage/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            DoFullImage = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ReferenceRingCurrent/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            bc = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/YCen/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            Ycen = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ZCen/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            Zcen = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/UpperBoundThreshold/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            IntSat = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/PixelSize/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            px = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/Width/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            Width = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/LayerNr/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(int);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            LayerNr = *(int *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/Wavelength/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            Wavelength = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/Lsd/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            Lsd = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/BadPxIntensity/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            BadPxIntensity = *(double *)&data[0];
            makeMap = 1;
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/MaxRingRad/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            RhoD = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/RhoD/0")!=NULL){
            arr = calloc(finfo->size + 1, sizeof(char)); 
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, arr, finfo->size);
            dsize = sizeof(double);
            data = (char*)malloc((size_t)dsize);
            dsize = blosc1_decompress(arr,data,dsize);
            RhoD = *(double *)&data[0];
            free(arr);
            free(data);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ImTransOpt/0")!=NULL){
            locImTransOpt = count;
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/RingThresh/0.0")!=NULL){
            locRingThresh = count;
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/RingThresh/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                sscanf(ptr3,"%*[^0123456789]%d",&nRingsThresh);
            } else return 1;
            free(s);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/OmegaRanges/0.0")!=NULL){
            locOmegaRanges = count;
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/OmegaRanges/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                sscanf(ptr3,"%*[^0123456789]%d",&nOmegaRanges);
            } else return 1;
            free(s);
        }
        if (strstr(finfo->name,"analysis/process/analysis_parameters/ImTransOpt/.zarray")!=NULL){
            s = calloc(finfo->size + 1, sizeof(char));
            fd = zip_fopen_index(arch, count, 0);
            zip_fread(fd, s, finfo->size);
            char *ptr = strstr(s,"shape");
            if (ptr != NULL){
                char *ptrt = strstr(ptr,"[");
                char *ptr2 = strstr(ptrt,"]");
                int loc = (int)(ptr2 - ptrt);
                char ptr3[2048];
                strncpy(ptr3,ptrt,loc+1);
                sscanf(ptr3,"%*[^0123456789]%d",&nImTransOpt);
            } else return 1;
            free(s);
        }
        count++;
    }
	int TransOpt[nImTransOpt], RingNrs[nRingsThresh];
    double Thresholds[nRingsThresh];
    // Read TransOpt
    zip_stat_index(arch, locImTransOpt, 0, finfo);
    s = calloc(finfo->size + 1, sizeof(char));
    fd = zip_fopen_index(arch, locImTransOpt, 0);
    zip_fread(fd, s, finfo->size); 
    dsize = nImTransOpt*sizeof(int);
    data = (char*)malloc((size_t)dsize);
    dsize = blosc1_decompress(s,data,dsize);
    int iter;
    for (iter=0;iter<nImTransOpt;iter++) TransOpt[iter] = *(int *)&data[iter*sizeof(int)];
    free(s);
    free(data);
    // Read RingNrs and Thresholds
    zip_stat_index(arch, locRingThresh, 0, finfo);
    s = calloc(finfo->size + 1, sizeof(char));
    fd = zip_fopen_index(arch, locRingThresh, 0);
    zip_fread(fd, s, finfo->size); 
    dsize = nRingsThresh*2*sizeof(double);
    data = (char*)malloc((size_t)dsize);
    dsize = blosc1_decompress(s,data,dsize);
    for (iter=0;iter<nRingsThresh;iter++){
        RingNrs[iter]    = (int) *(double *)&data[(iter*2+0)*sizeof(double)];
        Thresholds[iter] =       *(double *)&data[(iter*2+1)*sizeof(double)];
    }
    free(s);
    free(data);
    nFrames = nFrames - skipFrame; // This ensures we don't over-read.
    dataLoc += skipFrame;
    omegaStart += skipFrame*omegaStep;
    printf("%lf %d %lf\n",omegaStart,skipFrame,omegaStep);
	if (NrPixelsY != NrPixelsZ){
		if (NrPixelsY > NrPixelsZ){
			NrPixels = NrPixelsY;
		} else {
			NrPixels = NrPixelsZ;
		}
	} else {
        NrPixels = NrPixelsY;
	}
	Width = Width/px;
	int a,b;
	for (a=0;a<nImTransOpt;a++){
		if (TransOpt[a] < 0 || TransOpt[a] > 3){
			printf("TransformationOptions can only be 0, 1, 2 or 3.\nExiting.\n");
			return 0;
		}
		printf("TransformationOptions: %d ",TransOpt[a]);
		if (TransOpt[a] == 0)
			printf("No change.\n");
		else if (TransOpt[a] == 1)
			printf("Flip Left Right.\n");
		else if (TransOpt[a] == 2)
			printf("Flip Top Bottom.\n");
		else
			printf("Transpose.\n");
	}
    // Dark file reading from here.
	double *dark, *flood, *darkTemp;
	dark = calloc(NrPixels*NrPixels,sizeof(*dark));
	darkTemp = calloc(NrPixels*NrPixels,sizeof(*darkTemp));
	flood = calloc(NrPixels*NrPixels,sizeof(*flood));
	pixelvalue *darkcontents, *darkAsym;
	darkcontents = calloc(NrPixels*NrPixels,sizeof(*darkcontents));
	darkAsym = calloc(NrPixelsY*NrPixelsZ,sizeof(*darkAsym));
    int darkIter;
    dsize = bytesPerPx*NrPixelsZ*NrPixelsY;
    data = (char*)malloc((size_t)dsize);
    for (darkIter=0;darkIter<nDarks;darkIter++){
        zip_stat_index(arch, darkLoc, 0, finfo);
        // Go to the right location in the zip file and read frames.
        arr = calloc(finfo->size + 1, sizeof(char));
        fd = zip_fopen_index(arch, darkLoc, 0);
        zip_fread(fd, arr, finfo->size);
        dsize = blosc1_decompress(arr,data,dsize);
        free(arr);
        memcpy(darkAsym,(pixelvalue *)data,(size_t)dsize);
        MakeSquare(NrPixels,NrPixelsY,NrPixelsZ,darkAsym,darkcontents);
        DoImageTransformations(nImTransOpt,TransOpt,darkcontents,NrPixels);
        for (b=0;b<(NrPixels*NrPixels);b++){
            darkTemp[b] += darkcontents[b];
        }
    }
    if (nDarks > 0) for (a=0;a<(NrPixels*NrPixels);a++) darkTemp[a] /= nDarks;
	Transposer(darkTemp,NrPixels,dark);
    free(data);
	free(darkTemp);
	free(darkcontents);
	free(darkAsym);
    dsize = NrPixels*NrPixels*sizeof(double);
    if (nFloods>0){
        data = (char*)malloc((size_t)dsize);
        zip_stat_index(arch, floodLoc, 0, finfo);
        arr = calloc(finfo->size + 1, sizeof(char));
        fd = zip_fopen_index(arch, floodLoc, 0);
        zip_fread(fd, arr, finfo->size);
        dsize = blosc1_decompress(arr,data,dsize);
        free(arr);
        memcpy(flood,data,dsize);
    } else for(a=0;a<(NrPixels*NrPixels);a++) flood[a]=1;
    zip_close(arch);

	char OutFolderName[2048];
	sprintf(OutFolderName,"%s/%s",resultFolder,TmpFolder);
	int e = CheckDirectoryCreation(OutFolderName);
	if (e == 0){ return 1;}

	double txr, tyr, tzr;
	txr = deg2rad*tx;
	tyr = deg2rad*ty;
	tzr = deg2rad*tz;
	double Rx[3][3] = {{1,0,0},{0,cos(txr),-sin(txr)},{0,sin(txr),cos(txr)}};
	double Ry[3][3] = {{cos(tyr),0,sin(tyr)},{0,1,0},{-sin(tyr),0,cos(tyr)}};
	double Rz[3][3] = {{cos(tzr),-sin(tzr),0},{sin(tzr),cos(tzr),0},{0,0,1}};
	double TRint[3][3], TRs[3][3];
	MatrixMultF33(Ry,Rz,TRint);
	MatrixMultF33(Rx,TRint,TRs);
	// Get coordinates to process.
	int thisRingNr;
	double RingRads[nRingsThresh];
	char aliner[1000];
	int Rnr;
	double RRd;
	if (DoFullImage == 0){
		char hklfn[2040];
		sprintf(hklfn,"%s/hkls.csv",resultFolder);
		FILE *hklf = fopen(hklfn,"r");
		if (hklf == NULL){
			printf("HKL file could not be read. Exiting\n");
			return 1;
		}
		fgets(aliner,1000,hklf);
		while (fgets(aliner,1000,hklf)!=NULL){
			sscanf(aliner, "%s %s %s %s %d %s %s %s %s %s %lf",dummy,dummy,dummy,dummy,&Rnr,dummy,dummy,dummy,dummy,dummy,&RRd);
			for (thisRingNr=0;thisRingNr<nRingsThresh;thisRingNr++){
				if (Rnr == RingNrs[thisRingNr]){
					RingRads[thisRingNr] = RRd/px;
					break;
				}
			}
		}
	}
	double Rmin, Rmax;
	double Yc, Zc, n0=2, n1=4, n2=2;
	double ABC[3], ABCPr[3], XYZ[3];
	double Rad, Eta, RNorm, DistortFunc, EtaT, Rt;
	int nrCoords = 0;
	double *GoodCoords;
	GoodCoords = calloc(NrPixels*NrPixels,sizeof(*GoodCoords));
	if (DoFullImage == 1){
		for (a=0;a<NrPixels*NrPixels;a++){
			GoodCoords[a] = Thresholds[0];
		}
		nrCoords = NrPixels * NrPixels;
	} else {
		for (a=1;a<NrPixels;a++){
			for (b=1;b<NrPixels;b++){
				// Correct for tilts and Distortion here
				Yc = (-a + Ycen)*px;
				Zc =  (b - Zcen)*px;
				ABC[0] = 0;
				ABC[1] = Yc;
				ABC[2] = Zc;
				MatrixMult(TRs,ABC,ABCPr);
				XYZ[0] = Lsd+ABCPr[0];
				XYZ[1] = ABCPr[1];
				XYZ[2] = ABCPr[2];
				Rad = (Lsd/(XYZ[0]))*(sqrt(XYZ[1]*XYZ[1] + XYZ[2]*XYZ[2]));
				Eta = CalcEtaAngle(XYZ[1],XYZ[2]);
				RNorm = Rad/RhoD;
				EtaT = 90 - Eta;
				DistortFunc = (p0*(pow(RNorm,n0))*(cos(deg2rad*(2*EtaT)))) + (p1*(pow(RNorm,n1))*(cos(deg2rad*(4*EtaT+p3)))) + (p2*(pow(RNorm,n2))) + 1;
				Rt = Rad * DistortFunc / px;
				for (thisRingNr=0;thisRingNr<nRingsThresh;thisRingNr++){
					if (Rt > RingRads[thisRingNr] - Width && Rt < RingRads[thisRingNr] + Width){
						GoodCoords[((a-1)*NrPixels)+(b-1)] = Thresholds[thisRingNr];
						nrCoords ++;
					}
				}
			}
		}
	}
	printf("Number of coordinates: %d\n",nrCoords);

	// Allocate Arrays to hold other arrays
	size_t bigArrSize = NrPixels;
	bigArrSize *= NrPixels;
	bigArrSize *= numProcs;
	size_t asymBigArrSize = NrPixelsY;
	asymBigArrSize *= NrPixelsZ;
	asymBigArrSize *= numProcs;
	pixelvalue *ImageAll, *ImageAsymAll;
	double *ImgCorrBCAll, *ImgCorrBCTempAll, *MaxValAll, *zAll, *IntIntAll, *ImaxAll, *YcenAll, *ZcenAll, *RAll, *EtaAll, *OIAll;
	int *BoolImageAll, *ConnCompAll, *PosAll, *PosTrackersAll, *MaxPosAll, *UsefulPxAll, *NrPxAll;
	ImageAll = calloc(bigArrSize,sizeof(*ImageAll));
	ImageAsymAll = calloc(asymBigArrSize,sizeof(*ImageAsymAll));
	ImgCorrBCAll = calloc(bigArrSize,sizeof(*ImgCorrBCAll));
	ImgCorrBCTempAll = calloc(bigArrSize,sizeof(*ImgCorrBCTempAll));
	BoolImageAll = calloc(bigArrSize,sizeof(*BoolImageAll));
	ConnCompAll = calloc(bigArrSize,sizeof(*ConnCompAll));
	bigArrSize = nOverlapsMaxPerImage;
	bigArrSize *= NrPixels;
	bigArrSize *= 4;
	bigArrSize *= numProcs;
	PosAll = calloc(bigArrSize,sizeof(*PosAll));
	PosTrackersAll = calloc(nOverlapsMaxPerImage*numProcs,sizeof(*PosTrackersAll));
	MaxPosAll = calloc(NrPixels*20*numProcs,sizeof(*MaxPosAll));
	MaxValAll = calloc(NrPixels*10*numProcs,sizeof(*MaxValAll));
	UsefulPxAll = calloc(NrPixels*20*numProcs,sizeof(*UsefulPxAll));
	zAll = calloc(NrPixels*10*numProcs,sizeof(*zAll));
	IntIntAll = calloc(maxNPeaks*2*numProcs,sizeof(*IntIntAll));
	ImaxAll = calloc(maxNPeaks*2*numProcs,sizeof(*ImaxAll));
	YcenAll = calloc(maxNPeaks*2*numProcs,sizeof(*YcenAll));
	ZcenAll = calloc(maxNPeaks*2*numProcs,sizeof(*ZcenAll));
	RAll = calloc(maxNPeaks*2*numProcs,sizeof(*RAll));
	EtaAll = calloc(maxNPeaks*2*numProcs,sizeof(*EtaAll));
	OIAll = calloc(maxNPeaks*10*numProcs,sizeof(*OIAll));
	NrPxAll = calloc(maxNPeaks*2*numProcs,sizeof(*NrPxAll));

	int startFileNr = (int)(ceil((double)nFrames / (double)nBlocks)) * blockNr;
	int endFileNr = (int)(ceil((double)nFrames / (double)nBlocks)) * (blockNr+1) < nFrames ? (int)(ceil((double)nFrames / (double)nBlocks)) * (blockNr+1) : nFrames;
	int nrJobs = (int)(ceil((double)(endFileNr - startFileNr)/(double)(numProcs)));
	printf("StartFileNr: %d EndFileNr: %d numProcs: %d nrJobs/proc: %d blockNr: %d nrBlocks: %d\n",startFileNr,endFileNr,numProcs,nrJobs,blockNr,nBlocks);
	int nrFilesDone=0;
	int FileNr;
	# pragma omp parallel for num_threads(numProcs) private(FileNr) schedule(dynamic)
	for (FileNr = startFileNr; FileNr < endFileNr; FileNr++)
	{
		int procNum = omp_get_thread_num();
        printf("FrameNr: %d\n",FileNr);
        double beamcurr = 1;
		pixelvalue *Image, *ImageAsym;
		double *ImgCorrBCTemp, *ImgCorrBC, *MaximaValues, *z;
		double *IntegratedIntensity, *IMAX, *YCEN, *ZCEN, *Rads, *Etass, *OtherInfo;
		int *BoolImage, *ConnectedComponents, *Positions, *PositionTrackers, *MaximaPositions, *UsefulPixels, *NrPx;
		size_t idxoffset;
		idxoffset = NrPixels; idxoffset *= NrPixels; idxoffset *= procNum;
		size_t asym_idxoffset; asym_idxoffset = NrPixelsY; asym_idxoffset *= NrPixelsZ; asym_idxoffset *= procNum;
		Image = &ImageAll[idxoffset];
		ImageAsym = &ImageAsymAll[asym_idxoffset];
		ImgCorrBC = &ImgCorrBCAll[idxoffset];
		ImgCorrBCTemp = &ImgCorrBCTempAll[idxoffset];
		BoolImage = &BoolImageAll[idxoffset];
		ConnectedComponents = &ConnCompAll[idxoffset];
		idxoffset = nOverlapsMaxPerImage;
		idxoffset *= procNum;
		PositionTrackers = &PosTrackersAll[idxoffset];
		idxoffset = NrPixels; idxoffset *= 10; idxoffset *= procNum;
		MaximaValues = &MaxValAll[idxoffset];
		z = &zAll[idxoffset];
		idxoffset = nOverlapsMaxPerImage; idxoffset *= NrPixels; idxoffset *= 4; idxoffset *= procNum;
		Positions = &PosAll[idxoffset];
		idxoffset = NrPixels; idxoffset *= 20; idxoffset *= procNum;
		UsefulPixels = &UsefulPxAll[idxoffset];
		MaximaPositions = &MaxPosAll[idxoffset];
		idxoffset = maxNPeaks; idxoffset *= 2; idxoffset *= procNum;
		IntegratedIntensity = &IntIntAll[idxoffset];
		IMAX = &ImaxAll[idxoffset];
		YCEN = &YcenAll[idxoffset];
		ZCEN = &ZcenAll[idxoffset];
		Rads = &RAll[idxoffset];
		Etass = &EtaAll[idxoffset];
		NrPx = &NrPxAll[idxoffset];
		idxoffset *= 5;
		OtherInfo = &OIAll[idxoffset];
		#pragma omp critical
		{
			nrFilesDone++;
		}
		double Thresh;
		int i,j,k;
		double Omega;
		int Nadditions;
        Omega = omegaStart + FileNr*omegaStep;
		char OutFile[1024];
		sprintf(OutFile,"%s/%s_%06d_PS.csv",OutFolderName,DataFN,FileNr+1);
		FILE *outfilewrite;
		outfilewrite = fopen(OutFile,"w");
		fprintf(outfilewrite,"SpotID IntegratedIntensity Omega(degrees) YCen(px) ZCen(px) IMax Radius(px) Eta(degrees) SigmaR SigmaEta NrPixels TotalNrPixelsInPeakRegion nPeaks maxY maxZ diffY diffZ rawIMax returnCode\n");

        int32_t dsz = NrPixelsY*NrPixelsZ*bytesPerPx;
        char *locData = (char*)malloc((size_t)dsz);
        int errorpLoc = 0;
        zip_t* archLoc = NULL;
        archLoc = zip_open(DataFN,0,&errorpLoc);
        if (errorpLoc!=NULL) printf("File opening in parallel did not work. Undefined behavior now.\n");
        struct zip_stat* fifo = NULL;
        fifo = calloc(16384, sizeof(int));
        zip_stat_init(fifo);
        zip_file_t* fLoc = NULL;
        int dataLocThis = dataLoc + FileNr;
        zip_stat_index(archLoc,dataLocThis,0,fifo);
        char *locArr;
        locArr = calloc(fifo->size+1,sizeof(char));
        fLoc = zip_fopen_index(archLoc,dataLocThis,0);
        zip_fread(fLoc,locArr,fifo->size);
        dsz = blosc1_decompress(locArr,locData,dsz);
        // printf("%d %d\n",(int)dsz,(int)fifo->size);
        memcpy(ImageAsym,locData,dsz);
		MakeSquare(NrPixels,NrPixelsY,NrPixelsZ,ImageAsym,Image);
		if (makeMap == 1){
			int badPxCounter = 0;
			for (i=0;i<NrPixels*NrPixels;i++){
				if (Image[i] == (pixelvalue)BadPxIntensity){
					Image[i] = 0;
					badPxCounter++;
				}
			}
		}
        free(locData);
        free(fifo);
        zip_fclose(fLoc);
        free(locArr);
        zip_close(archLoc);

		DoImageTransformations(nImTransOpt,TransOpt,Image,NrPixels);
		for (i=0;i<(NrPixels*NrPixels);i++) ImgCorrBCTemp[i]=Image[i];
		Transposer(ImgCorrBCTemp,NrPixels,ImgCorrBC);
		for (i=0;i<(NrPixels*NrPixels);i++){
			if (GoodCoords[i] == 0){
				ImgCorrBC[i] = 0;
			} else {
				ImgCorrBC[i] = (ImgCorrBC[i] - dark[i])/flood[i];
				ImgCorrBC[i] = ImgCorrBC[i]*bc/beamcurr;
				if (ImgCorrBC[i] < GoodCoords[i]){
					ImgCorrBC[i] = 0;
				}
			}
		}
		// Do Connected components
		int NrOfReg;
		for (i=0;i<NrPixels*NrPixels;i++){
			if (ImgCorrBC[i] != 0){
				BoolImage[i] = 1;
			}else{
				BoolImage[i] = 0;
			}
		}
		for (i=0;i<nOverlapsMaxPerImage;i++){
			PositionTrackers[i] = 0;
			for (j=0;j<NrPixels*4;j++){
				Positions[i*NrPixels*4+j] = 0;
			}
		}
		NrOfReg = FindConnectedComponents(BoolImage,NrPixels,ConnectedComponents,Positions,PositionTrackers);
		// printf("FrameNr: %d, NrOfRegions: %d\n",FileNr,NrOfReg);
		int RegNr,NrPixelsThisRegion;
		int IsSaturated;
		int SpotIDStart = 1;
		int TotNrRegions = NrOfReg;
		for (i=0;i<NrPixels*10;i++){
			MaximaPositions[i*2+0] = 0;
			MaximaPositions[i*2+1] = 0;
			MaximaValues[i] = 0;
			UsefulPixels[i*2+0] = 0;
			UsefulPixels[i*2+1] = 0;
			z[i] = 0;
		}
		for (RegNr=1;RegNr<=NrOfReg;RegNr++){
			NrPixelsThisRegion = PositionTrackers[RegNr];
			for (i=0;i<NrPixelsThisRegion;i++){
				UsefulPixels[i*2+0] = (int)(Positions[RegNr*NrPixels*4+i]/NrPixels);
				UsefulPixels[i*2+1] = (int)(Positions[RegNr*NrPixels*4+i]%NrPixels);
				z[i] = ImgCorrBC[((UsefulPixels[i*2+0])*NrPixels) + (UsefulPixels[i*2+1])];
			}
			Thresh = GoodCoords[((UsefulPixels[0*2+0])*NrPixels) + (UsefulPixels[0*2+1])];
			unsigned nPeaks;
			nPeaks = FindRegionalMaxima(z,UsefulPixels,NrPixelsThisRegion,MaximaPositions,MaximaValues,&IsSaturated,IntSat,NrPixels);
			// printf("RegionNr: %d, NPeaks: %d\n",RegNr,nPeaks);
			if (NrPixelsThisRegion <= minNrPx || NrPixelsThisRegion >= maxNrPx){
				TotNrRegions--;
				continue;
			}
			if (IsSaturated == 1){ //Saturated peaks removed
				TotNrRegions--;
				continue;
			}
			if (nPeaks > maxNPeaks){
				// Sort peaks by MaxIntensity, remove the smallest peaks until maxNPeaks, arrays needed MaximaPositions, MaximaValues.
				int MaximaPositionsT[nPeaks*2];
				double MaximaValuesT[nPeaks];
				double maxIntMax;
				int maxPos;
				for (i=0;i<maxNPeaks;i++){
					maxIntMax = 0;
					for (j=0;j<nPeaks;j++){
						if (MaximaValues[j] > maxIntMax){
							maxPos = j;
							maxIntMax = MaximaValues[j];
						}
					}
					MaximaPositionsT[i*2+0] = MaximaPositions[maxPos*2+0];
					MaximaPositionsT[i*2+1] = MaximaPositions[maxPos*2+1];
					MaximaValuesT[i] = MaximaValues[maxPos];
					MaximaValues[maxPos] = 0;
				}
				nPeaks = maxNPeaks;
				for (i=0;i<nPeaks;i++){
					MaximaValues[i] = MaximaValuesT[i];
					MaximaPositions[i*2+0] = MaximaPositionsT[i*2+0];
					MaximaPositions[i*2+1] = MaximaPositionsT[i*2+1];
				}
			}
			int rc = Fit2DPeaks(nPeaks,NrPixelsThisRegion,z,UsefulPixels,MaximaValues,MaximaPositions,IntegratedIntensity,IMAX,YCEN,ZCEN,Rads,Etass,Ycen,Zcen,Thresh,NrPx,OtherInfo,NrPixels);
			for (i=0;i<nPeaks;i++){
				fprintf(outfilewrite,"%d %f %f %f %f %f %f %f ",(SpotIDStart+i),IntegratedIntensity[i],Omega,-YCEN[i]+Ycen,ZCEN[i]+Zcen,IMAX[i],Rads[i],Etass[i]);
				for (j=0;j<2;j++) fprintf(outfilewrite, "%f ",OtherInfo[2*i+j]);
				fprintf(outfilewrite,"%d %d %d %d %d %f %f %f %d\n",NrPx[i],NrPixelsThisRegion,nPeaks,MaximaPositions[i*2+0],MaximaPositions[i*2+1],(double)MaximaPositions[i*2+0]-YCEN[i]-Ycen,(double)MaximaPositions[i*2+1]-ZCEN[i]-Zcen,MaximaValues[i],rc);
			}
			SpotIDStart += nPeaks;
		}
		fclose(outfilewrite);
	}

	free(ImageAll);
	free(ImageAsymAll);
	free(ImgCorrBCAll);
	free(ImgCorrBCTempAll);
	free(BoolImageAll);
	free(ConnCompAll);
	free(PosAll);
	free(PosTrackersAll);
	free(MaxPosAll);
	free(MaxValAll);
	free(UsefulPxAll);
	free(zAll);
	free(IntIntAll);
	free(ImaxAll);
	free(YcenAll);
	free(ZcenAll);
	free(RAll);
	free(EtaAll);
	free(OIAll);
	free(NrPxAll);
	free(GoodCoords);
	free(dark);
	free(flood);
	double time = omp_get_wtime() - start_time;
	printf("Finished, time elapsed: %lf seconds, nrFramesDone: %d.\n",time,nrFilesDone);
    blosc2_destroy();
	return 0;
}