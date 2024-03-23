import subprocess
import numpy as np
import argparse
import warnings
import time
import os,sys,glob
from pathlib import Path
import shutil
from math import floor, isnan
import pandas as pd
from parsl.app.app import python_app
import parsl
from skimage.transform import iradon
from PIL import Image
from skimage.filters import threshold_otsu
from skimage.morphology import reconstruction
import matplotlib.pyplot as plt
import h5py
utilsDir = os.path.expanduser('~/opt/MIDAS/utils/')
sys.path.insert(0,utilsDir)
v7Dir = os.path.expanduser('~/opt/MIDAS/FF_HEDM/v7/')
sys.path.insert(0,v7Dir)
from calcMiso import *
import warnings
warnings.filterwarnings('ignore')

pytpath = 'python '
env = dict(os.environ)
midas_path = os.path.expanduser("~/.MIDAS")
env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'

def generateZip(resFol,pfn,layerNr,dfn='',dloc='',nchunks=-1,preproc=-1,outf='ZipOut.txt',errf='ZipErr.txt'):
	cmd = pytpath+' '+os.path.expanduser('~/opt/MIDAS/utils/ffGenerateZip.py')+' -resultFolder '+ resFol[:-1] +' -paramFN ' + pfn + ' -LayerNr ' + str(layerNr)
	if dfn!='':
		cmd+= ' -dataFN ' + dfn
	if dloc!='':
		cmd+= ' -dataLoc ' + dloc
	if nchunks!=-1:
		cmd+= ' -numFrameChunks '+str(nchunks)
	if preproc!=-1:
		cmd+= ' -preProcThresh '+str(preproc)
	outf = resFol+'/output/'+outf
	errf = resFol+'/output/'+errf
	subprocess.call(cmd,shell=True,stdout=open(outf,'w'),stderr=open(errf,'w'))
	lines = open(outf,'r').readlines()
	if lines[-1].startswith('OutputZipName'):
		return lines[-1].split()[1]

@python_app
def peaks(resultDir,zipFN,numProcs,blockNr=0,numBlocks=1):
    import subprocess
    import os
    env = dict(os.environ)
    midas_path = os.path.expanduser("~/.MIDAS")
    env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'
    f = open(f'{resultDir}/output/peaksearch_out{blockNr}.csv','w')
    f_err = open(f'{resultDir}/output/peaksearch_err{blockNr}.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/PeaksFittingOMPZarr")+f' {zipFN} {blockNr} {numBlocks} {numProcs}',shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()

@python_app
def indexscanning(resultDir,numProcs,num_scans,blockNr=0,numBlocks=1):
    import subprocess
    import os
    os.chdir(resultDir)
    env = dict(os.environ)
    midas_path = os.path.expanduser("~/.MIDAS")
    env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'
    f = open(f'{resultDir}/output/indexing_out{blockNr}.csv','w')
    f_err = open(f'{resultDir}/output/indexing_err{blockNr}.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/IndexerScanningOMP")+f' paramstest.txt {blockNr} {numBlocks} {num_scans} {numProcs}',shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()

@python_app
def refinescanning(resultDir,numProcs,blockNr=0,numBlocks=1):
    import subprocess
    import os
    os.chdir(resultDir)
    env = dict(os.environ)
    midas_path = os.path.expanduser("~/.MIDAS")
    env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'
    with open("SpotsToIndex.csv", "r") as f:
        num_lines = len(f.readlines())
    print(num_lines)
    cmd = os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitOrStrainsScanningOMP")+f' paramstest.txt {blockNr} {numBlocks} {num_lines} {numProcs}'
    print(cmd)
    f = open(f'{resultDir}/output/refining_out{blockNr}.csv','w')
    f_err = open(f'{resultDir}/output/refining_err{blockNr}.csv','w')
    subprocess.call(cmd,shell=True,env=env,cwd=resultDir,stdout=f,stderr=f_err)
    f.close()
    f_err.close()

class MyParser(argparse.ArgumentParser):
	def error(self, message):
		sys.stderr.write('error: %s\n' % message)
		self.print_help()
		sys.exit(2)

startTime = time.time()

warnings.filterwarnings('ignore')
parser = MyParser(description='''
PF_MIDAS, contact hsharma@anl.gov 
Provide positions.csv file (negative positions with respect to actual motor position, 
				  motor position is normally position of the rotation axis, opposite to the voxel position).
Parameter file and positions.csv file must be in the same folder.
''', formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('-nCPUs', type=int, required=False, default=32, help='Number of CPUs to use')
parser.add_argument('-nCPUsLocal', type=int, required=False, default=4, help='Local Number of CPUs to use')
parser.add_argument('-paramFile', type=str, required=True, help='ParameterFileName: Do not use the full path.')
parser.add_argument('-nNodes', type=str, required=False, default=1, help='Number of Nodes')
parser.add_argument('-machineName', type=str, required=False, default='local', help='Machine Name: local,orthrosall,orthrosnew,umich')
parser.add_argument('-doPeakSearch',type=int,required=True,help='0 if PeakSearch is already done. InputAllExtra...0..n.csv should exist in the folder')
parser.add_argument('-oneSolPerVox',type=int,required=True,help='0 if want to allow multiple solutions per voxel. 1 if want to have only 1 solution per voxel.')
parser.add_argument('-resultDir',type=str,required=False,default='',help='Directory where you want to save the results. If ommitted, the current directory will be used.')
parser.add_argument('-numFrameChunks', type=int, required=False, default=-1, help='If low on RAM, it can process parts of the dataset at the time. -1 will disable.')
parser.add_argument('-preProcThresh', type=int, required=False, default=-1, help='If want to save the dark corrected data, then put to whatever threshold wanted above dark. -1 will disable. 0 will just subtract dark. Negative values will be reset to 0.')
parser.add_argument('-DoTomo', type=int, required=False, default=0, help='If want to do tomography, put to 1. Only for OneSolPerVox.')
parser.add_argument('-ConvertFiles', type=int, required=False, default=1, help='If want to convert to zarr, if zarr files exist already, put to 0.')
parser.add_argument('-runIndexing', type=int, required=False, default=1, help='If want to skip Indexing, put to 0.')
args, unparsed = parser.parse_known_args()
baseNameParamFN = args.paramFile
machineName = args.machineName
doPeakSearch = args.doPeakSearch
oneSolPerVox = args.oneSolPerVox
numProcs = args.nCPUs
numProcsLocal = args.nCPUsLocal
nNodes = args.nNodes
topdir = args.resultDir
nchunks = args.numFrameChunks
preproc = args.preProcThresh
doTomo = args.DoTomo
ConvertFiles = args.ConvertFiles
runIndexing = args.runIndexing

if len(topdir) == 0:
	topdir = os.getcwd()

logDir = topdir + '/output'

os.makedirs(topdir,exist_ok=True)
os.makedirs(logDir,exist_ok=True)

if machineName == 'local':
	numNodes = 1
	from localConfig import *
	parsl.load(config=localConfig)
elif machineName == 'orthrosnew':
	pytpath = os.path.expanduser("~/opt/midasconda3/bin/python")
	os.environ['MIDAS_SCRIPT_DIR'] = logDir
	nNodes = 11
	numProcs = 32
	from orthrosAllConfig import *
	parsl.load(config=orthrosNewConfig)
elif machineName == 'orthrosall':
	pytpath = os.path.expanduser("~/opt/midasconda3/bin/python")
	os.environ['MIDAS_SCRIPT_DIR'] = logDir
	nNodes = 5
	numProcs = 64
	from orthrosAllConfig import *
	parsl.load(config=orthrosAllConfig)
elif machineName == 'umich':
	pytpath = '/nfs/turbo/meche-abucsek/Wenxi/ESRF_Ti_v7/.venf/bin'
	os.environ['MIDAS_SCRIPT_DIR'] = logDir
	os.environ['nNodes'] = nNodes
	numProcs = 36
	from uMichConfig import *
	parsl.load(config=uMichConfig)
elif machineName == 'marquette':
	os.environ['MIDAS_SCRIPT_DIR'] = logDir
	os.environ['nNodes'] = nNodes
	from marquetteConfig import *
	parsl.load(config=marquetteConfig)

paramContents = open(baseNameParamFN).readlines()
RingNrs = []
nMerges = 0
omegaOffset = 0
micFN = ''
maxang = 1
tol_ome = 1
tol_eta = 1
for line in paramContents:
	if line.startswith('StartFileNrFirstLayer'):
		startNrFirstLayer = int(line.split()[1])
	if line.startswith('MaxAng'):
		maxang = float(line.split()[1])
	if line.startswith('TolEta'):
		tol_eta = float(line.split()[1])
	if line.startswith('TolOme'):
		tol_ome = float(line.split()[1])
	if line.startswith('NrFilesPerSweep'):
		nrFilesPerSweep = int(line.split()[1])
	if line.startswith('MicFile'):
		micFN = line.split()[1]
	if line.startswith('FileStem'):
		fStem = line.split()[1]
	if line.startswith('Ext'):
		Ext = line.split()[1]
	if line.startswith('StartNr'):
		startNr = int(line.split()[1])
	if line.startswith('EndNr'):
		endNr = int(line.split()[1])
	if line.startswith('SpaceGroup'):
		sgnum = int(line.split()[1])
	if line.startswith('nStepsToMerge'):
		nMerges = int(line.split()[1])
	if line.startswith('omegaOffsetBetweenScans'):
		omegaOffset = float(line.split()[1])
	if line.startswith('nScans'):
		nScans = int(line.split()[1])
	if line.startswith('Lsd'):
		Lsd = float(line.split()[1])
	if line.startswith('OverAllRingToIndex'):
		RingToIndex = int(line.split()[1])
	if line.startswith('BeamSize'):
		BeamSize = float(line.split()[1])
	if line.startswith('OmegaStep'):
		omegaStep = float(line.split()[1])
	if line.startswith('px'):
		px = float(line.split()[1])
	if line.startswith('RingThresh'):
		RingNrs.append(int(line.split()[1]))

subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/GetHKLList")+' ' + baseNameParamFN,shell=True)
hkls = np.genfromtxt('hkls.csv',skip_header=1)
_,idx = np.unique(hkls[:,4],return_index=True)
hkls = hkls[idx,:]
rads = [hkl[-1] for rnr in RingNrs for hkl in hkls if hkl[4] == rnr]
print(RingNrs)
print(rads)

if doPeakSearch == 1:
	positions = open(topdir+'/positions.csv').readlines()
	for layerNr in range(1,nScans+1):
		print(f'LayerNr: {layerNr}')
		ypos = float(positions[layerNr-1])
		thisStartNr = startNrFirstLayer + (layerNr-1)*nrFilesPerSweep
		folderName = str(thisStartNr)
		thisDir = topdir + '/' + folderName + '/'
		Path(thisDir).mkdir(parents=True,exist_ok=True)
		os.chdir(thisDir)
		thisParamFN = thisDir + baseNameParamFN
		thisPF = open(thisParamFN,'w')
		for line in paramContents:
			thisPF.write(line)
		thisPF.close()
		Path(thisDir+'/Temp').mkdir(parents=True,exist_ok=True)
		Path(thisDir+'/output').mkdir(parents=True,exist_ok=True)
		sub_logDir = thisDir + '/output'
		if ConvertFiles==1:
			outFStem = generateZip(thisDir,baseNameParamFN,layerNr,nchunks=nchunks,preproc=preproc)
		else:
			outFStem = f'{thisDir}/{fStem}_{str(thisStartNr).zfill(6)}{Ext}'
		print(outFStem)
		subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/GetHKLListZarr")+" "+outFStem,env=env,shell=True)
		res = []
		for nodeNr in range(nNodes):
			res.append(peaks(thisDir,outFStem,numProcs,blockNr=nodeNr,numBlocks=nNodes))
		outputs = [i.result() for i in res]
		if omegaOffset != 0:
			fns = glob.glob('Temp/*PS.csv')
			for fn in fns:
				df = pd.read_csv(fn,delimiter=' ')
				if df.shape[0] == 0:
					continue
				omega_this = df['Omega(degrees)'][0]
				omegaOffsetThis = omegaOffset*layerNr
				omegaOffsetThis = omegaOffsetThis%360.0
				omega_new = omega_this - omegaOffsetThis
				df['Omega(degrees)'] = omega_new
				df.to_csv(fn,sep=' ',header=True,float_format='%.6f',index=False)
		subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/MergeOverlappingPeaksAllZarr")+' '+outFStem,env=env,shell=True)
		subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/CalcRadiusAllZarr")+' '+outFStem,env=env,shell=True)
		subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitSetupZarr")+' '+outFStem,env=env,shell=True)
		Result = np.genfromtxt(f'Radius_StartNr_{startNr}_EndNr_{endNr}.csv',skip_header=1,delimiter=' ')
		if len(Result.shape)<2:
			shutil.copy2('InputAllExtraInfoFittingAll.csv',topdir+'/InputAllExtraInfoFittingAll'+str(layerNr-1)+'.csv')
			continue
		uniqueRings,uniqueIndices = np.unique(Result[:,13],return_index=True)
		ringPowderIntensity = []
		for iter in range(len(uniqueIndices)):
			ringPowderIntensity.append([uniqueRings[iter],Result[uniqueIndices[iter],16]])
		ringPowderIntensity = np.array(ringPowderIntensity)
		dfAllF = pd.read_csv('InputAllExtraInfoFittingAll.csv',delimiter=' ',skipinitialspace=True)
		dfAllF.loc[dfAllF['GrainRadius']>0.001,'%YLab'] += ypos
		dfAllF.loc[dfAllF['GrainRadius']>0.001,'YOrig(NoWedgeCorr)'] += ypos
		dfAllF['Eta'] = CalcEtaAngleAll(dfAllF['%YLab'],dfAllF['ZLab'])
		dfAllF['Ttheta'] = rad2deg*np.arctan(np.linalg.norm(np.array([dfAllF['%YLab'],dfAllF['ZLab']]),axis=0)/Lsd)
		for iter in range(len(ringPowderIntensity)):
			ringNr = ringPowderIntensity[iter,0]
			powInt = ringPowderIntensity[iter,1]
			dfAllF.loc[dfAllF['RingNumber']==ringNr,'GrainRadius'] *= powInt**(1/3)
		outFN2 = topdir+'/InputAllExtraInfoFittingAll'+str(layerNr-1)+'.csv'
		dfAllF.to_csv(outFN2,sep=' ',header=True,float_format='%.6f',index=False)
		shutil.copy2(thisDir+'/paramstest.txt',topdir+'/paramstest.txt')
		shutil.copy2(thisDir+'/hkls.csv',topdir+'/hkls.csv')
else:
	if nMerges!=0:
		os.chdir(topdir)
		if os.path.exists('original_positions.csv'):
			shutil.move('original_positions.csv','positions.csv')
		positions = open(topdir+'/positions.csv').readlines()
		for layerNr in range(0,nMerges*(nScans//nMerges)):
			if os.path.exists(f'original_InputAllExtraInfoFittingAll{layerNr}.csv'):
				shutil.move(f'original_InputAllExtraInfoFittingAll{layerNr}.csv',f'InputAllExtraInfoFittingAll{layerNr}.csv')

if nMerges != 0:
	shutil.move('positions.csv','original_positions.csv')
	for layerNr in range(0,nMerges*(nScans//nMerges)):
		if os.path.exists(f'InputAllExtraInfoFittingAll{layerNr}.csv'):
			shutil.move(f'InputAllExtraInfoFittingAll{layerNr}.csv',f'original_InputAllExtraInfoFittingAll{layerNr}.csv')
	subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/mergeScansScanning")+f" {nMerges*(nScans//nMerges)} {nMerges} {2*px} {2*omegaStep} {numProcsLocal}",shell=True)
	positions = open(topdir+'/positions.csv').readlines()
	nScans = int(floor(nScans / nMerges))

positions = open(topdir+'/positions.csv').readlines()
os.chdir(topdir)
Path(topdir+'/Output').mkdir(parents=True,exist_ok=True)
Path(topdir+'/Results').mkdir(parents=True,exist_ok=True)
paramsf = open('paramstest.txt','r')
lines = paramsf.readlines()
paramsf.close()
paramsf = open('paramstest.txt','w')
for line in lines:
	if line.startswith('RingNumbers'):
		continue
	if line.startswith('RingRadii'):
		continue
	if line.startswith('RingToIndex'):
		continue
	if line.startswith('BeamSize'):
		continue
	if line.startswith('px'):
		continue
	if line.startswith('OutputFolder'):
		paramsf.write('OutputFolder '+topdir+'/Output\n')
	elif line.startswith('ResultFolder'):
		paramsf.write('ResultFolder '+topdir+'/Results\n')
	else:
		paramsf.write(line)
for idx in range(len(RingNrs)):
	paramsf.write('RingNumbers '+str(RingNrs[idx])+'\n')
	paramsf.write('RingRadii '+str(rads[idx])+'\n')
paramsf.write('BeamSize '+str(BeamSize)+'\n')
paramsf.write('px '+str(px)+'\n')
paramsf.write('RingToIndex '+str(RingToIndex)+'\n')
if len(micFN) > 0:
	paramsf.write(f'MicFile {micFN}\n')
paramsf.close()

if (runIndexing == 1):
	subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/SaveBinDataScanning")+' '+str(nScans),shell=True)
	resIndex = []
	for nodeNr in range(nNodes):
		resIndex.append(indexscanning(topdir,numProcs,nScans,blockNr=nodeNr,numBlocks=nNodes))
	outputIndex = [i.result() for i in resIndex]

if oneSolPerVox==1:
	# subprocess.call(os.path.expanduser('~/opt/MIDAS/FF_HEDM/bin/findSingleSolutionPF')+f' {topdir} {sgnum} {maxang} {nScans} {numProcsLocal} {tol_ome} {tol_eta}',cwd=topdir,shell=True)
	if doTomo == 1:
		sinoFN = glob.glob("sinos_*.bin")[0]
		nGrs = int(sinoFN.split('_')[1])
		maxNHKLs = int(sinoFN.split('_')[2])
		Sinos = np.fromfile(sinoFN,dtype=np.double,count=nGrs*maxNHKLs*nScans).reshape((nGrs,maxNHKLs,nScans))
		omegas = np.fromfile(f"omegas_{nGrs}_{maxNHKLs}.bin",dtype=np.double,count=nGrs*maxNHKLs).reshape((nGrs,maxNHKLs))
		grainSpots = np.fromfile(f"nrHKLs_{nGrs}.bin",dtype=np.int32,count=nGrs)

		os.makedirs('Sinos',exist_ok=True)
		os.makedirs('Thetas',exist_ok=True)
		os.makedirs('Recons',exist_ok=True)

		all_recons = np.zeros((nGrs,nScans,nScans))
		im_list = []
		for grNr in range(nGrs):
			nSp = grainSpots[grNr]
			thetas = omegas[grNr,:nSp]
			sino = np.transpose(Sinos[grNr,:nSp,:])
			Image.fromarray(sino).save('Sinos/sino_grNr_'+str.zfill(str(grNr),4)+'.tif')
			np.savetxt('Thetas/thetas_grNr_'+str.zfill(str(grNr),4)+'.txt',thetas,fmt='%.6f')
			recon = iradon(sino,theta=thetas)
			all_recons[grNr,:,:] = recon
			im_list.append(Image.fromarray(recon))
			Image.fromarray(recon).save('Recons/recon_grNr_'+str.zfill(str(grNr),4)+'.tif')

		full_recon = np.max(all_recons,axis=0)
		print("Done with tomo recon, now running the optimization.")
		max_id = np.argmax(all_recons,axis=0).astype(np.int32)
		max_id[full_recon==0] = -1
		Image.fromarray(max_id).save('Recons/Full_recon_max_project_grID.tif')
		Image.fromarray(full_recon).save('Recons/Full_recon_max_project.tif')
		im_list[0].save('Recons/all_recons_together.tif',compression="tiff_deflate",save_all=True,append_images=im_list[1:])
		uniqueOrientations = np.genfromtxt(f'{topdir}/UniqueOrientations.csv',delimiter=' ')
		f = open(f'{topdir}/SpotsToIndex.csv','w')
		for voxNr in range(nScans*nScans):
			locX = voxNr % nScans - 1
			locY = nScans - (voxNr//nScans + 1)
			if (voxNr == 2398):
				print(max_id[locY,locX])
				print(locX,locY)
			# print(locX,locY,voxNr)
			if max_id[locX,locY] == -1:
				continue
			orientThis = uniqueOrientations[max_id[locY,locX],5:]
			if os.path.isfile(f'{topdir}/Output/UniqueIndexKeyOrientAll_voxNr_{voxNr}.txt'):
				with open(f'{topdir}/Output/UniqueIndexKeyOrientAll_voxNr_{voxNr}.txt','r') as f:
					lines = f.readlines()
				for line in lines:
					if (voxNr == 2398):
						print(orientThis,orientInside)
					orientInside = [float(val) for val in line.split()[4:]]
					ang = rad2deg*GetMisOrientationAngleOM(orientThis,orientInside,sgnum)[0]
					if ang < maxang:
						print(line)
						f.write(line)
						break
		f.close()
	else:
		f = open(f'{topdir}/SpotsToIndex.csv','w')
		idData = np.fromfile(f'{topdir}/Output/UniqueIndexSingleKey.bin',dtype=np.uintp,count=nScans*nScans*5).reshape((-1,5))
		for voxNr in range(nScans*nScans):
			if idData[voxNr,1] !=0:
				f.write(f"{idData[voxNr,0]} {idData[voxNr,1]} {idData[voxNr,2]} {idData[voxNr,3]} {idData[voxNr,4]}\n")
		f.close()

	os.makedirs('Results',exist_ok=True)
	resRefine = []
	for nodeNr in range(nNodes):
		resRefine.append(refinescanning(topdir,numProcs,blockNr=nodeNr,numBlocks=nNodes))
	outputRefine = [i.result() for i in resRefine]
	
	NrSym,Sym = MakeSymmetries(sgnum)
	print(f"Filtering the final output. Will be saved to {topdir}/Recons/microstrFull.csv and {topdir}/Recons/microstructure.hdf")

	files2 = glob.glob(topdir+'/Results/*.csv')
	filesdata = np.zeros((len(files2),43))
	i=0
	info_arr = np.zeros((23,nScans*nScans))
	info_arr[:,:] = np.nan
	for fileN in files2:
		f = open(fileN)
		voxNr = int(fileN.split('.')[-2].split('_')[-2])
		_ = f.readline()
		data = f.readline().split()
		for j in range(len(data)):
			filesdata[i][j] = float(data[j])
		OM = filesdata[i][1:10]
		quat = BringDownToFundamentalRegionSym(OrientMat2Quat(OM),NrSym,Sym)
		if isnan(filesdata[i][26]):
			continue
		if filesdata[i][26] < 0 or filesdata[i][26] > 1.0000000001:
			continue
		filesdata[i][39:43] = quat
		info_arr[:,voxNr] = filesdata[i][[0,-4,-3,-2,-1,11,12,15,16,17,18,19,20,22,23,24,26,27,28,29,31,32,35]]
		i+=1
		f.close()
	head = 'SpotID,O11,O12,O13,O21,O22,O23,O31,O32,O33,SpotID,x,y,z,SpotID,a,b,c,alpha,beta,gamma,SpotID,PosErr,OmeErr,InternalAngle,'
	head += 'Radius,Completeness,E11,E12,E13,E21,E22,E23,E31,E32,E33,Eul1,Eul2,Eul3,Quat1,Quat2,Quat3,Quat4'
	np.savetxt('Recons/microstrFull.csv',filesdata,fmt='%.6f',delimiter=',',header=head)
	f = h5py.File('Recons/microstructure.hdf','w')
	micstr = f.create_dataset(name='microstr',dtype=np.double,data=filesdata)
	micstr.attrs['Header'] = np.string_(head)
	info_arr = info_arr.reshape((23,nScans,nScans))
	info_arr = np.flip(info_arr,axis=(1,2))
	info_arr = info_arr.transpose(0,2,1)
	imgs = f.create_dataset(name='images',dtype=np.double,data=info_arr)
	imgs.attrs['Header'] = np.string_('ID,Quat1,Quat2,Quat3,Quat4,x,y,a,b,c,alpha,beta,gamma,posErr,omeErr,InternalAngle,Completeness,E11,E12,E13,E22,E23,E33')
	f.close()
else:
	subprocess.call(os.path.expanduser('~/opt/MIDAS/FF_HEDM/bin/findMultipleSolutionsPF')+f' {topdir} {sgnum} {maxang} {nScans} {numProcsLocal}',shell=True,cwd=topdir)
	print("Now running refinement for all solutions found.")
	resRefine = []
	for nodeNr in range(nNodes):
		resRefine.append(refinescanning(topdir,numProcs,blockNr=nodeNr,numBlocks=nNodes))
	outputRefine = [inter.result() for inter in resRefine]

	NrSym,Sym = MakeSymmetries(sgnum)
	files2 = glob.glob(topdir+'/Results/*.csv')
	filesdata = np.zeros((len(files2),43))
	i=0
	for fileN in files2:
		f = open(fileN)
		str1 = f.readline()
		data = f.readline().split()
		for j in range(len(data)):
			filesdata[i][j] = float(data[j])
		OM = filesdata[i][1:10]
		quat = BringDownToFundamentalRegionSym(OrientMat2Quat(OM),NrSym,Sym)
		filesdata[i][39:43] = quat
		i+=1
		f.close()
	head = 'SpotID,O11,O12,O13,O21,O22,O23,O31,O32,O33,SpotID,x,y,z,SpotID,a,b,c,alpha,beta,gamma,SpotID,PosErr,OmeErr,InternalAngle,Radius,Completeness,'
	head += 'E11,E12,E13,E21,E22,E23,E31,E32,E33,Eul1,Eul2,Eul3,Quat1,Quat2,Quat3,Quat4'
	np.savetxt('microstrFull.csv',filesdata,fmt='%.6f',delimiter=',',header=head)

print("Done. Time Elapsed: "+str(time.time()-startTime)+" seconds.")
