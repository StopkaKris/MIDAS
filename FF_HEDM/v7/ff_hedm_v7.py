import subprocess
import sys, os
from pprint import pprint as print
import time

t0 = time.time()

env = dict(os.environ)
midas_path = os.path.expanduser("~/.MIDAS")
env['LD_LIBRARY_PATH'] = f'{midas_path}/BLOSC/lib64:{midas_path}/FFTW/lib:{midas_path}/HDF5/lib:{midas_path}/LIBTIFF/lib:{midas_path}/LIBZIP/lib64:{midas_path}/NLOPT/lib:{midas_path}/ZLIB/lib'

if len(sys.argv)<3:
    print("Provide a (required)parameter file, (optional)data file and (required)numProcs to use.")
    sys.exit()
elif len(sys.argv)==4:
    psFN = sys.argv[1]
    dataFN = sys.argv[2]
    numProcs = int(sys.argv[3])
    strRun = f' {psFN} {dataFN}'
    print("Generating combined MIDAS file from HDF and ps files.")
elif len(sys.argv)==3:
    psFN = sys.argv[1]
    numProcs = int(sys.argv[2])
    strRun = f' {psFN}'
    print("Generating combined MIDAS file from GE and ps files.")
f_ge2h5 = open('ff2midas_out.csv','w')
subprocess.call('python '+os.path.expanduser("~/opt/MIDAS/utils/ff2midas.py")+strRun,shell=True,stdout=f_ge2h5)
f_ge2h5.close()
f_ge2h5 = open('ff2midas_out.csv','r')
lines = f_ge2h5.readlines()
f_ge2h5.close()
for line in lines:
    if line.startswith('Out: '):
        outFStem = line.split()[1]
    if line.startswith('ResultDir: '):
        resultDir = line.split()[1]
startdir = os.getcwd()
print(f"Generating ZarrZip file. Time till now: {time.time()-t0}")
f_h52zarr = open('midas2zip_out.csv','w')
subprocess.call('python '+os.path.expanduser("~/opt/MIDAS/utils/midas2zip.py")+' '+outFStem,shell=True,stdout=f_h52zarr)
f_h52zarr.close()

print(f'Cleanup of: {outFStem}')
os.remove(f'{outFStem}')

print(f"Generating HKLs. Time till now: {time.time()-t0}")
f_hkls = open('hkls_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/GetHKLListZarr")+' '+outFStem+'.zip',shell=True,env=env,stdout=f_hkls)
f_hkls.close()
print(f"Doing PeakSearch. Time till now: {time.time()-t0}")
f = open('peaksearch_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/PeaksFittingOMPZarr")+' '+outFStem+'.zip 0 1 '+str(numProcs),shell=True,env=env,stdout=f)
f.close()
print(f"Merging peaks. Time till now: {time.time()-t0}")
f = open('merge_overlaps_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/MergeOverlappingPeaksAllZarr")+' '+outFStem+'.zip',shell=True,env=env,stdout=f)
f.close()
print(f"Calculating Radii. Time till now: {time.time()-t0}")
f = open('calc_radius_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/CalcRadiusAllZarr")+' '+outFStem+'.zip',shell=True,env=env,stdout=f)
f.close()
print(f"Transforming data. Time till now: {time.time()-t0}")
f = open('fit_setup_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitSetupZarr")+' '+outFStem+'.zip',shell=True,env=env,stdout=f)
f.close()
os.chdir(resultDir)
print(f"Binning data. Time till now: {time.time()-t0}")
f = open(f'{startdir}/binning_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/SaveBinData")+' paramstest.txt',shell=True,env=env,stdout=f)
f.close()
with open("SpotsToIndex.csv", "r") as f:
    num_lines = len(f.readlines())

print(f"Indexing. Time till now: {time.time()-t0}")
f = open(f'{startdir}/indexing_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/IndexerOMP")+' paramstest.txt 0 1 '+str(num_lines)+' '+str(numProcs),shell=True,env=env,stdout=f)
f.close()
print(f"Refining. Time till now: {time.time()-t0}")
f = open(f'{startdir}/refinement_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/FitPosOrStrainsOMP")+' paramstest.txt 0 1 '+str(num_lines)+' '+str(numProcs),shell=True,env=env,stdout=f)
f.close()
print(f"Making grains list. Time till now: {time.time()-t0}")
f = open(f'{startdir}/process_grains_out.csv','w')
subprocess.call(os.path.expanduser("~/opt/MIDAS/FF_HEDM/bin/ProcessGrainsZarr")+' '+outFStem+'.zip',shell=True,env=env,stdout=f)
f.close()
print(f"Done. Total time elapsed: {time.time()-t0}")