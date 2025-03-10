#
# Copyright 2022 Intel Corporation.
#
# SPDX-License-Identifier: Apache-2.0

from libcpp cimport bool
from libcpp.memory cimport shared_ptr, unique_ptr
from libcpp.string cimport string
from libcpp.vector cimport vector
from libcpp.map cimport map

from pyarrow.lib cimport CTable as CArrowTable

from pyhdk._common cimport CSQLTypeInfo, CSystemParameters

cdef extern from "omniscidb/DataMgr/MemoryLevel.h" namespace "Data_Namespace":
  enum MemoryLevel:
    DISK_LEVEL = 0,
    CPU_LEVEL = 1,
    GPU_LEVEL = 2,


cdef extern from "omniscidb/SchemaMgr/TableInfo.h":
  cdef cppclass CTableRef "TableRef":
    int db_id
    int table_id

    TableRef(int, int)

  cdef cppclass CTableInfo "TableInfo"(CTableRef):
    string name
    bool is_view
    MemoryLevel persistence_level
    size_t fragments
    bool is_stream

    CTableInfo(int, int, string, bool, MemoryLevel, size_t, bool);
    string toString()

ctypedef shared_ptr[CTableInfo] CTableInfoPtr
ctypedef vector[CTableInfoPtr] CTableInfoList

cdef extern from "omniscidb/SchemaMgr/ColumnInfo.h":
  cdef cppclass CColumnRef "ColumnRef":
    int db_id
    int table_id
    int column_id

    CColumnRef(int, int, int)

  cdef cppclass CColumnInfo "ColumnInfo"(CColumnRef):
    string name
    CSQLTypeInfo type
    bool is_rowid

    CColumnInfo(int, int, int, string, CSQLTypeInfo, bool)
    string toString()

ctypedef shared_ptr[CColumnInfo] CColumnInfoPtr
ctypedef vector[CColumnInfoPtr] CColumnInfoList

cdef extern from "omniscidb/SchemaMgr/SchemaProvider.h":
  cdef cppclass CSchemaProvider "SchemaProvider":
    int getId()
    vector[int] listDatabases()
    CTableInfoList listTables(int)
    CColumnInfoList listColumns(int, int)
    CTableInfoPtr getTableInfo(int, int)
    CTableInfoPtr getTableInfoByName "getTableInfo"(int, string&)
    CColumnInfoPtr getColumnInfo(int, int, int)
    CColumnInfoPtr getColumnInfoByName "getColumnInfo"(int, int, string&)

ctypedef shared_ptr[CSchemaProvider] CSchemaProviderPtr

cdef class TableInfo:
  cdef CTableInfoPtr c_table_info

cdef class ColumnInfo:
  cdef CColumnInfoPtr c_column_info

cdef class SchemaProvider:
  cdef shared_ptr[CSchemaProvider] c_schema_provider

  cpdef getId(self)
  cpdef listDatabases(self)
  cpdef listTables(self, db)
  cpdef listColumns(self, db, table)
  cpdef getTableInfo(self, db, table)
  cpdef getColumnInfo(self, db, table, column)

cdef extern from "omniscidb/DataMgr/AbstractBufferMgr.h" namespace "Data_Namespace":
  cdef cppclass CAbstractBufferMgr "AbstractBufferMgr":
    pass

cdef extern from "omniscidb/DataMgr/AbstractDataProvider.h":
  cdef cppclass CAbstractDataProvider "AbstractDataProvider"(CAbstractBufferMgr):
    pass

cdef extern from "omniscidb/ArrowStorage/ArrowStorage.h" namespace "ArrowStorage":
  struct CColumnDescription "ArrowStorage::ColumnDescription":
    string name;
    CSQLTypeInfo type;

  struct CTableOptions "ArrowStorage::TableOptions":
    size_t fragment_size;

  struct CCsvParseOptions "ArrowStorage::CsvParseOptions":
    char delimiter;
    bool header;
    size_t skip_rows;
    size_t block_size;

  struct CJsonParseOptions "ArrowStorage::JsonParseOptions":
    size_t skip_rows;
    size_t block_size;

cdef class Storage(SchemaProvider):
  cdef shared_ptr[CAbstractBufferMgr] c_abstract_buffer_mgr

cdef extern from "omniscidb/ArrowStorage/ArrowStorage.h":
  cdef cppclass CArrowStorage "ArrowStorage"(CSchemaProvider, CAbstractDataProvider):
    CArrowStorage(int, string, int);

    CTableInfoPtr importArrowTable(shared_ptr[CArrowTable], string&, CTableOptions&);
    void dropTable(const string&, bool)

cdef extern from "omniscidb/BufferProvider/BufferProvider.h" namespace "Data_Namespace":
  cdef cppclass CBufferProvider "BufferProvider":
    pass

cdef extern from "omniscidb/DataProvider/DataProvider.h":
  cdef cppclass CDataProvider "DataProvider":
    pass

cdef extern from "omniscidb/DataMgr/GpuMgr.h":
  cdef enum CGpuMgrName "GpuMgrName":
    CUDA,
    L0,

  cdef cppclass CGpuMgr "GpuMgr":
    pass

cdef extern from "omniscidb/DataMgr/PersistentStorageMgr/PersistentStorageMgr.h":
  cdef cppclass CPersistentStorageMgr "PersistentStorageMgr"(CAbstractBufferMgr):
    void registerDataProvider(int, shared_ptr[CAbstractBufferMgr]);

cdef extern from "omniscidb/DataMgr/DataMgr.h" namespace "Data_Namespace":
  cdef cppclass CDataMgr "DataMgr":
    CDataMgr(const string&, const CSystemParameters&, map[CGpuMgrName, unique_ptr[CGpuMgr]]&& gpuMgrs, size_t reservedGpuMem, size_t numReaderThreads)

    CPersistentStorageMgr* getPersistentStorageMgr()
    CBufferProvider* getBufferProvider()
    CDataProvider* getDataProvider()

cdef class DataMgr:
  cdef shared_ptr[CDataMgr] c_data_mgr

  cpdef registerDataProvider(self, Storage storage)
