# Modified code from https://gist.github.com/rsignell-usgs/49c214c9aaab4935e15a83bf3e228d03/revisions
# Adapted by Hemant Sharma to copy over all HDF5 datasets, no linking.

import logging
import numpy as np
import h5py
import zarr
from zarr.meta import encode_fill_value
import fsspec
import sys
import shutil
from pathlib import Path
from pprint import pprint as print
from numcodecs import Blosc #Default compression

lggr = logging.getLogger('h5-to-zarr')
lggr.addHandler(logging.NullHandler())

class Hdf5ToZarr:
    """Translate the content of one HDF5 file into Zarr metadata.
    HDF5 groups become Zarr groups. HDF5 datasets become Zarr arrays. Zarr array
    is copied over from the HDF5 file.
    Parameters
    ----------
    h5f : file-like or str
        Input HDF5 file as a string or file-like Python object.
    store : MutableMapping
        Zarr store.
    xarray : bool, optional
        Produce atributes required by the `xarray <http://xarray.pydata.org>`_
        package to correctly identify dimensions (HDF5 dimension scales) of a
        Zarr array. Default is ``False``.
    """

    def __init__(self, h5f, store, xarray=False):
        # Open HDF5 file in read mode...
        lggr.debug(f'HDF5 file: {h5f}')
        lggr.debug(f'Zarr store: {store}')
        lggr.debug(f'xarray: {xarray}')
        self._h5f = h5py.File(h5f, mode='r')
        self._xr = xarray

        # Create Zarr store's root group...
        self._zroot = zarr.group(store=store, overwrite=True)

    def translate(self):
        """Translate content of one HDF5 file into Zarr storage format.
        All data is copied out of the HDF5 file as Blosc compressed arrays.
        """
        lggr.debug('Translation begins')
        self.transfer_attrs(self._h5f, self._zroot)
        self._h5f.visititems(self.translator)

    def transfer_attrs(self, h5obj, zobj):
        """Transfer attributes from an HDF5 object to its equivalent Zarr object.
        Parameters
        ----------
        h5obj : h5py.Group or h5py.Dataset
            An HDF5 group or dataset.
        zobj : zarr.hierarchy.Group or zarr.core.Array
            An equivalent Zarr group or array to the HDF5 group or dataset with
            attributes.
        """
        for n, v in h5obj.attrs.items():
            # Fix some attribute values to avoid JSON encoding exceptions...
            if isinstance(v, bytes):
                v = v.decode('utf-8')
            elif isinstance(v, (np.ndarray, np.number)):
                if n == '_FillValue':
                    v = encode_fill_value(v, v.dtype)
                elif v.size == 1:
                    v = v.flatten()[0].tolist()
                else:
                    v = v.tolist()

            try:
                zobj.attrs[n] = v
            except TypeError:
                print(f'Caught TypeError: {n}@{h5obj.name} = {v} ({type(v)})')

    def translator(self, name, h5obj):
        """Produce Zarr metadata for all groups and datasets in the HDF5 file.
        """
        if isinstance(h5obj, h5py.Dataset):
            lggr.debug(f'Dataset: {h5obj.name}')
            compressor = Blosc(cname='zstd', clevel=3, shuffle=Blosc.BITSHUFFLE)

            # Create a Zarr array equivalent to this HDF5 dataset...
            za = self._zroot.create_dataset(h5obj.name, shape=h5obj.shape,
                                            dtype=h5obj.dtype,
                                            chunks=h5obj.chunks or False,
                                            fill_value=h5obj.fillvalue,
                                            compression=compressor,
                                            overwrite=True)
            lggr.debug(f'Created Zarr array: {za}')
            self.transfer_attrs(h5obj, za)

            if self._xr:
                # Do this for xarray...
                adims = self._get_array_dims(h5obj)
                za.attrs['_ARRAY_DIMENSIONS'] = adims
                lggr.debug(f'_ARRAY_DIMENSIONS = {adims}')

            za[:] = h5obj[()]

        elif isinstance(h5obj, h5py.Group):
            lggr.debug(f'Group: {h5obj.name}')
            zgrp = self._zroot.create_group(h5obj.name)
            self.transfer_attrs(h5obj, zgrp)

    def _get_array_dims(self, dset):
        """Get a list of dimension scale names attached to input HDF5 dataset.
        This is required by the xarray package to work with Zarr arrays. Only
        one dimension scale per dataset dimension is allowed. If dataset is
        dimension scale, it will be considered as the dimension to itself.
        Parameters
        ----------
        dset : h5py.Dataset
            HDF5 dataset.
        Returns
        -------
        list
            List with HDF5 path names of dimension scales attached to input
            dataset.
        """
        dims = list()
        rank = len(dset.shape)
        if rank:
            for n in range(rank):
                num_scales = len(dset.dims[n])
                if num_scales == 1:
                    dims.append(dset.dims[n][0].name[1:])
                elif h5py.h5ds.is_scale(dset.id):
                    dims.append(dset.name[1:])
                elif num_scales > 1:
                    raise RuntimeError(
                        f'{dset.name}: {len(dset.dims[n])} '
                        f'dimension scales attached to dimension #{n}')
        return dims

    def storage_info(self, dset):
        """Get storage information of an HDF5 dataset in the HDF5 file.
        Storage information consists of file offset and size (length) for every
        chunk of the HDF5 dataset.
        Parameters
        ----------
        dset : h5py.Dataset
            HDF5 dataset for which to collect storage information.
        Returns
        -------
        dict
            HDF5 dataset storage information. Dict keys are chunk array offsets
            as tuples. Dict values are pairs with chunk file offset and size
            integers.
        """
        # Empty (null) dataset...
        if dset.shape is None:
            return dict()

        dsid = dset.id
        if dset.chunks is None:
            # Contiguous dataset...
            if dsid.get_offset() is None:
                # No data ever written...
                return dict()
            else:
                key = (0,) * (len(dset.shape) or 1)
                return {key: {'offset': dsid.get_offset(),
                              'size': dsid.get_storage_size()}}
        else:
            # Chunked dataset...
            num_chunks = dsid.get_num_chunks()
            if num_chunks == 0:
                # No data ever written...
                return dict()

            # Go over all the dataset chunks...
            stinfo = dict()
            chunk_size = dset.chunks
            for index in range(num_chunks):
                blob = dsid.get_chunk_info(index)
                key = tuple(
                    [a // b for a, b in zip(blob.chunk_offset, chunk_size)])
                stinfo[key] = {'offset': blob.byte_offset,
                               'size': blob.size}
            return stinfo

if __name__ == '__main__':
    lggr.setLevel(logging.DEBUG)
    lggr_handler = logging.StreamHandler()
    lggr_handler.setFormatter(logging.Formatter(
        '%(levelname)s:%(name)s:%(funcName)s:%(message)s'))
    lggr.addHandler(lggr_handler)
    fn = sys.argv[1]
    outzip = '.'.join(fn.split('h5')[:-1])+'zip'
    zipF = Path(outzip)
    if zipF.exists():
        shutil.move(outzip,outzip+'.old')

    with fsspec.open(fn,mode='rb', anon=False, requester_pays=True,default_fill_cache=False) as f:
        storeZip = zarr.ZipStore(outzip)
        h5chunkszip = Hdf5ToZarr(f, storeZip, xarray=True)
        h5chunkszip.translate()

# Consolidate Zarr metadata...
lggr.info('Consolidating Zarr dataset metadata')
zarr.convenience.consolidate_metadata(storeZip)
lggr.info('Done')
lggr.info('Written file tree-structure:')
storeZip.close()
print(zarr.open(outzip).tree())