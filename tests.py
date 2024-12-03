#!/usr/bin/env python3

import unittest
import subprocess
import os
import time
import shutil
import tempfile
import hashlib
import random
import string

SOURCE_FILE = "xxxxxx"

class FuseFilesystemTest(unittest.TestCase):
    FUSE_PROGRAM = './build/splinterfs'  
    MOUNTPOINT = '/tmp/' + ''.join(random.choices(string.ascii_letters, k=8))
    SPLIT_SIZE = 100048576
    SOURCE_FILE = SOURCE_FILE
    BASE_NAME = os.path.basename(SOURCE_FILE)
    FUSE_PID = None

    @classmethod
    def setUpClass(cls):
        # Create mountpoint directory
        os.makedirs(cls.MOUNTPOINT, exist_ok=True)

        # Start FUSE filesystem
        cls.fuse_process = subprocess.Popen([cls.FUSE_PROGRAM, cls.SOURCE_FILE, cls.MOUNTPOINT])
        cls.FUSE_PID = cls.fuse_process.pid
        time.sleep(2)  # Wait for filesystem to initialize

    @classmethod
    def tearDownClass(cls):
        # Unmount the filesystem
        subprocess.run(['fusermount', '-u', cls.MOUNTPOINT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.fuse_process.wait()

        # Remove mountpoint directory
        shutil.rmtree(cls.MOUNTPOINT, ignore_errors=True)

        # Cleanup temporary files
        temp_files = ['/tmp/combined_splits', '/tmp/write_test_output', '/tmp/non_existent_test_output', '/tmp/dd_output']
        for f in temp_files:
            try:
                os.remove(f)
            except FileNotFoundError:
                pass

    def test_1_mounting(self):
        # Check if mounted
        result = subprocess.run(['mount'], stdout=subprocess.PIPE)
        mounted = self.MOUNTPOINT in result.stdout.decode()
        self.assertTrue(mounted, "Filesystem did not mount successfully.")

    def test_2_directory_listing(self):
        # List root directory contents
        files = os.listdir(self.MOUNTPOINT)

        source_size = os.stat(self.SOURCE_FILE).st_size
        expected_splits = (source_size + self.SPLIT_SIZE - 1) // self.SPLIT_SIZE

        self.assertEqual(len(files), expected_splits, f"Expected {expected_splits} splits, found {len(files)}.")

        expected_files = [f"{i}_{self.BASE_NAME}" for i in range(expected_splits)]
        self.assertListEqual(sorted(files), sorted(expected_files), "Split file names do not match expected pattern.")

    def test_3_file_attributes(self):
        files = os.listdir(self.MOUNTPOINT)
        source_size = os.stat(self.SOURCE_FILE).st_size
        expected_splits = (source_size + self.SPLIT_SIZE - 1) // self.SPLIT_SIZE

        for i, file in enumerate(sorted(files)):
            file_path = os.path.join(self.MOUNTPOINT, file)
            st = os.stat(file_path)

            # Check permissions
            perms = oct(st.st_mode & 0o777)
            self.assertEqual(perms, '0o444', f"Incorrect permissions on {file} (found {perms}).")

            if i < expected_splits - 1:
                expected_size = self.SPLIT_SIZE
            else:
                expected_size = source_size - self.SPLIT_SIZE * (expected_splits - 1)
            self.assertEqual(st.st_size, expected_size, f"Incorrect size on {file} (expected {expected_size}, found {st.st_size}).")

    def test_4_reading_split_files(self):
        # Copy each split file to /tmp
        files = os.listdir(self.MOUNTPOINT)
        for file in files:
            src = os.path.join(self.MOUNTPOINT, file)
            dst = os.path.join('/tmp', f'split_{file}')
            shutil.copyfile(src, dst)

        # Concatenate split files
        with open('/tmp/combined_splits', 'wb') as outfile:
            for file in sorted(files, key=lambda x: int(x.split('_')[0])):
                with open(os.path.join('/tmp', f'split_{file}'), 'rb') as infile:
                    outfile.write(infile.read())

        # Compare with original source file
        with open(self.SOURCE_FILE, 'rb') as f1, open('/tmp/combined_splits', 'rb') as f2:
            self.assertEqual(f1.read(), f2.read(), "Combined splits do not match the source file.")

    def test_5_read_beyond_eof(self):
        # Identify the last split file
        files = os.listdir(self.MOUNTPOINT)
        last_split = sorted(files, key=lambda x: int(x.split('_')[0]))[-1]
        file_path = os.path.join(self.MOUNTPOINT, last_split)

        # Attempt to read beyond EOF
        with open(file_path, 'rb') as f:
            f.seek(self.SPLIT_SIZE)
            data = f.read(1)
            self.assertEqual(len(data), 0, "Data read beyond EOF.")

    def test_6_write_access_denied(self):
        # Try to write to a split file
        first_file = sorted(os.listdir(self.MOUNTPOINT))[0]
        file_path = os.path.join(self.MOUNTPOINT, first_file)

        with self.assertRaises(PermissionError, msg="Write operation succeeded unexpectedly."):
            with open(file_path, 'wb') as f:
                f.write(b'Test')

    def test_7_access_non_existent_file(self):
        # Attempt to access a non-existent file
        non_existent_file = os.path.join(self.MOUNTPOINT, 'non_existent_file')
        with self.assertRaises(FileNotFoundError, msg="Non-existent file accessed unexpectedly."):
            os.stat(non_existent_file)

class FuseFilesystemDynamicTest(unittest.TestCase):
    FUSE_PROGRAM = './build/splinterfs'
    SOURCE_FILE = SOURCE_FILE
    MOUNTPOINT = '/tmp/' + ''.join(random.choices(string.ascii_letters, k=8))
    SPLIT_SIZE = 100048576

    def setUp(self):
        # Create mountpoint directory
        os.makedirs(self.MOUNTPOINT, exist_ok=True)

    def tearDown(self):
        # Remove mountpoint directory
        shutil.rmtree(self.MOUNTPOINT, ignore_errors=True)
        # Cleanup temporary files
        temp_files = ['/tmp/combined_splits_dynamic']
        for f in temp_files:
            try:
                os.remove(f)
            except FileNotFoundError:
                pass

    def test_dynamic_source_file_change(self):
        # Create a temporary file which is a copy of the source file
        with tempfile.NamedTemporaryFile(delete=False) as temp_source_file:
            temp_source_file_path = temp_source_file.name
        shutil.copyfile(self.SOURCE_FILE, temp_source_file_path)

        # Start the FUSE filesystem on the temporary file
        fuse_process = subprocess.Popen([self.FUSE_PROGRAM, temp_source_file_path, self.MOUNTPOINT])
        time.sleep(2)  # Wait for filesystem to initialize

        try:
            # Append the original source file to the temporary file
            with open(temp_source_file_path, 'ab') as temp_file, open(self.SOURCE_FILE, 'rb') as source_file:
                temp_file.write(source_file.read())

            # Wait a moment to ensure the filesystem notices the change
            time.sleep(1)

            # Read all the splits from the mountpoint and combine them into one file
            files = os.listdir(self.MOUNTPOINT)
            combined_splits_path = '/tmp/combined_splits_dynamic'

            with open(combined_splits_path, 'wb') as outfile:
                for file in sorted(files, key=lambda x: int(x.split('_')[0])):
                    with open(os.path.join(self.MOUNTPOINT, file), 'rb') as infile:
                        outfile.write(infile.read())

            with open(combined_splits_path, 'rb') as f:
                combined_splits_hash = hashlib.sha256(f.read()).hexdigest()

            with open(temp_source_file_path, 'rb') as f:
                temp_source_file_hash = hashlib.sha256(f.read()).hexdigest()

            # Compare the two hashes
            self.assertEqual(combined_splits_hash, temp_source_file_hash,
                             "Hashes do not match after source file change.")

        finally:
            # Unmount the filesystem
            subprocess.run(['fusermount', '-u', self.MOUNTPOINT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            fuse_process.wait()
            # Remove temporary files
            os.remove(temp_source_file_path)
            if os.path.exists(combined_splits_path):
                os.remove(combined_splits_path)

if __name__ == '__main__':
    unittest.main()
