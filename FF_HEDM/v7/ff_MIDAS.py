import parsl
import subprocess
import sys, os
from pprint import pprint as print
import time
import argparse
import signal
utilsDir = os.path.expanduser('~/opt/MIDAS/utils/')
sys.path.insert(0,utilsDir)
v7Dir = os.path.expanduser('~/opt/MIDAS/FF_HEDM/v7/')
sys.path.insert(0,v7Dir)
from ff_apps import *

default_handler = None

def handler(num, frame):    
    subprocess.call("rm -rf /dev/shm/*.bin",shell=True)
    print("Ctrl-C was pressed, cleaning up.")
    return default_handler(num, frame) 

class MyParser(argparse.ArgumentParser):
    def error(self, message):
        sys.stderr.write('error: %s\n' % message)
        self.print_help()
        sys.exit(2)

default_handler = signal.getsignal(signal.SIGINT)
signal.signal(signal.SIGINT, handler)
parser = MyParser(description='''Far-field HEDM analysis using MIDAS. V7.0.0, contact hsharma@anl.gov''', formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('-resultFolder', type=str, required=True, help='Folder where you want to save results')
parser.add_argument('-paramFN', type=str, required=True, help='Parameter file name')
parser.add_argument('-dataFN', type=str, required=False, default='', help='DataFileName')
parser.add_argument('-nCPUs', type=int, required=True, help='Number of CPU cores to use')
parser.add_argument('-machineName', type=str, required=False, default='local', help='Machine name for execution')
parser.add_argument('-numFrameChunks', type=int, required=False, default=-1, help='If low on RAM, it can process parts of the dataset at the time. -1 will disable.')
parser.add_argument('-preProcThresh', type=int, required=False, default=-1, help='If want to save the dark corrected data, then put to whatever threshold wanted above dark. -1 will disable. 0 will just subtract dark. Negative values will be reset to 0.')
parser.add_argument('-nNodes', type=int, required=False, default=1, help='Number of nodes for execution')
parser.add_argument('-LayerNr', type=int, required=False, default=1, help='LayerNr to process')
args, unparsed = parser.parse_known_args()
resultDir = args.resultFolder
psFN = args.paramFN
dataFN = args.dataFN
numProcs = args.nCPUs
machineName = args.machineName
nNodes = args.nNodes
nchunks = args.numFrameChunks
preproc = args.preProcThresh
layerNr = args.LayerNr

resultDir += f'/LayerNr_{layerNr}'

os.makedirs(resultDir,exist_ok=True)

t0 = time.time()

env = dict(os.environ)
midas_path = os.path.expanduser("~/.MIDAS")
env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'

if len(dataFN)>0:
    strRun = f' -paramFN={psFN} -dataFN={dataFN} -resultFolder={resultDir}'
    print("Generating combined MIDAS file from HDF and ps files.")
else:
    strRun = f' -paramFN={psFN} -resultFolder={resultDir}'
    print("Generating combined MIDAS file from GE and ps files.")

if machineName == 'local':
    from localConfig import *
    parsl.load(config=localConfig)
    outFStem = generateZip(resultDir,psFN,layerNr,dfn=dataFN,nchunks=nchunks,preproc=preproc)
    print(f"Generating HKLs. Time till now: {time.time()-t0} seconds.")
    f_hkls = open(f'{resultDir}/hkls_out.csv','w')
    f_hkls_err = open(f'{resultDir}/hkls_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/GetHKLListZarr")+' '+outFStem,shell=True,env=env,stdout=f_hkls,stderr=f_hkls_err)
    f_hkls.close()
    f_hkls_err.close()
    print(f"Doing PeakSearch. Time till now: {time.time()-t0} seconds.")
    res = peaks(resultDir,outFStem,numProcs,f_hkls_err)
    res.result()
    print(f"Merging peaks. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/merge_overlaps_out.csv','w')
    f_err = open(f'{resultDir}/merge_overlaps_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/MergeOverlappingPeaksAllZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Calculating Radii. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/calc_radius_out.csv','w')
    f_err = open(f'{resultDir}/calc_radius_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/CalcRadiusAllZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Transforming data. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/fit_setup_out.csv','w')
    f_err = open(f'{resultDir}/fit_setup_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitSetupZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    os.chdir(resultDir)
    print(f"Binning data. Time till now: {time.time()-t0}")
    f2 = open(f'binning_out.csv','w')
    f_err2 = open(f'binning_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/SaveBinData")+' paramstest.txt',shell=True,env=env,stdout=f2,stderr=f_err2)
    f2.close()
    f_err2.close()
    print(f"Indexing. Time till now: {time.time()-t0}")
    resIndex = index(resultDir,numProcs,f_err2)
    resIndex.result()
    print(f"Refining. Time till now: {time.time()-t0}")
    resRefine = refine(resultDir,numProcs,resIndex)
    resRefine.result()
    subprocess.call("rm -rf /dev/shm/*.bin",shell=True)
    print(f"Making grains list. Time till now: {time.time()-t0}")
    f = open(f'process_grains_out.csv','w')
    f_err = open(f'process_grains_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/ProcessGrainsZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Making plots, condensing output. Time till now: {time.time()-t0}")
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotFFSpots3d.py')+' -resultFolder '+resultDir,cwd=resultDir, shell=True)
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotFFSpots3dGrains.py')+' -resultFolder '+resultDir,cwd=resultDir,shell=True)
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotGrains3d.py')+' -resultFolder '+resultDir,cwd=resultDir,shell=True)
    print(f"Done. Total time elapsed: {time.time()-t0}")
else:
    if machineName == 'orthrosall':
        nNodes = 11
        numProcs = 32
        from orthrosAllConfig import *
        parsl.load(config=orthrosAllConfig)
    outFStem = generateZip(resultDir,psFN,layerNr,dfn=dataFN,nchunks=nchunks,preproc=preproc)
    print(f"Generating HKLs. Time till now: {time.time()-t0} seconds.")
    f_hkls = open(f'{resultDir}/hkls_out.csv','w')
    f_hkls_err = open(f'{resultDir}/hkls_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/GetHKLListZarr")+' '+outFStem,shell=True,env=env,stdout=f_hkls,stderr=f_hkls_err)
    f_hkls.close()
    f_hkls_err.close()
    print(f"Doing PeakSearch. Time till now: {time.time()-t0} seconds.")
    res = []
    for nodeNr in range(nNodes):
        res.append(peaks(resultDir,outFStem,numProcs,f_hkls_err,blockNr=nodeNr,nBlocks=nNodes))
    outputs = [i.result() for i in res]
    print(f"Merging peaks. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/merge_overlaps_out.csv','w')
    f_err = open(f'{resultDir}/merge_overlaps_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/MergeOverlappingPeaksAllZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Calculating Radii. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/calc_radius_out.csv','w')
    f_err = open(f'{resultDir}/calc_radius_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/CalcRadiusAllZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Transforming data. Time till now: {time.time()-t0}")
    f = open(f'{resultDir}/fit_setup_out.csv','w')
    f_err = open(f'{resultDir}/fit_setup_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitSetupZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    os.chdir(resultDir)
    print(f"Binning data. Time till now: {time.time()-t0}")
    f2 = open(f'binning_out.csv','w')
    f_err2 = open(f'binning_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/SaveBinData")+' paramstest.txt',shell=True,env=env,stdout=f2,stderr=f_err2)
    f2.close()
    f_err2.close()
    print(f"Indexing. Time till now: {time.time()-t0}")
    resIndex = []
    for nodeNr in range(nNodes):
        resIndex.append(index(resultDir,numProcs,f_err2,blockNr=nodeNr,nBlocks=nNodes))
    outputIndex = [i.result() for i in resIndex]
    print(f"Refining. Time till now: {time.time()-t0}")
    resRefine = []
    resRefine.append(refine(resultDir,numProcs,outputIndex,blockNr=nodeNr,nBlocks=nNodes))
    outputRefine = [i.result() for i in resRefine]
    subprocess.call("rm -rf /dev/shm/*.bin",shell=True)
    print(f"Making grains list. Time till now: {time.time()-t0}")
    f = open(f'process_grains_out.csv','w')
    f_err = open(f'process_grains_err.csv','w')
    subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/ProcessGrainsZarr")+' '+outFStem,shell=True,env=env,stdout=f,stderr=f_err)
    f.close()
    f_err.close()
    print(f"Making plots, condensing output. Time till now: {time.time()-t0}")
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotFFSpots3d.py')+' -resultFolder '+resultDir,cwd=resultDir, shell=True)
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotFFSpots3dGrains.py')+' -resultFolder '+resultDir,cwd=resultDir,shell=True)
    subprocess.call('python '+os.path.expanduser('~/opt/MIDAS/utils/plotGrains3d.py')+' -resultFolder '+resultDir,cwd=resultDir,shell=True)
    print(f"Done. Total time elapsed: {time.time()-t0}")

# python ~/opt/MIDAS/FF_HEDM/v7/ff_MIDAS.py -resultFolder /data/tomo1/krause_mar23_midas/ff/recon -paramFN ps_dmi.txt -nCPUs 32 -machineName orthrosall -LayerNr 1