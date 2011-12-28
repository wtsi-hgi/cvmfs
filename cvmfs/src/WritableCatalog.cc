#include "WritableCatalog.h"

#include <iostream>
#include <sstream>

using namespace std;

namespace cvmfs {
  
bool WritableCatalog::CreateNewCatalogDatabase(const std::string &file_path,
                                               const DirectoryEntry &root_entry,
                                               const std::string &root_entry_parent_path,
                                               const bool root_catalog) {
  // create database schema for new catalog
  if (not WritableCatalog::CreateNewDatabaseSchema(file_path)) {
    pmesg(D_CATALOG, "failed to create database schema for new catalog '%s'", file_path.c_str());
    return false;
  }

  // configure the root entry
  hash::t_md5 path_hash;
  hash::t_md5 parent_hash;
  string root_path;
  if (root_catalog) {
    root_path = root_entry.name();
    path_hash = hash::t_md5(root_path);
    parent_hash = hash::t_md5();
  } else {
    root_path = root_entry_parent_path + "/" + root_entry.name();
    path_hash = hash::t_md5(root_path);
    parent_hash = hash::t_md5(root_entry_parent_path);
  }
  
  // open the new catalog temporarily to insert the root entry
  // we do not specify the parent directory here!
  WritableCatalog *new_catalog = new WritableCatalog(root_path, NULL);
  if (not new_catalog->OpenDatabase(file_path)) {
    pmesg(D_CATALOG, "opening new catalog '%s' for the first time failed.", file_path.c_str());
    return false;
  }
  
  // add the root entry to the new catalog
  pmesg(D_CATALOG, "inserting root entry '%s' into new catalog '%s'", root_path.c_str(), new_catalog->path().c_str());
  if (not new_catalog->AddEntry(root_entry, path_hash, parent_hash)) {
    pmesg(D_CATALOG, "inserting root entry in new catalog '%s' failed", file_path.c_str());
    return false;
  }
  
  // close the newly created catalog
  delete new_catalog;
  
  return true;
}

bool WritableCatalog::CreateNewDatabaseSchema(const std::string &file_path) {
  sqlite3 *database;
  int open_flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  
  // create the directory structure for this catalog
  if (not mkdir_deep(get_parent_path(file_path), plain_dir_mode)) {
    pmesg(D_CATALOG, "cannot create pseudo directory structure for new nested catalog database file '%s'", file_path.c_str());
    return false;
  }
  
  // create the new catalog file and open it
  pmesg(D_CATALOG, "creating new catalog at '%s'", file_path.c_str());
  if (SQLITE_OK != sqlite3_open_v2(file_path.c_str(), &database, open_flags, NULL)) {
    pmesg(D_CATALOG, "Cannot create and open catalog database file '%s'", file_path.c_str());
    return false;
  }
  sqlite3_extended_result_codes(database, 1);
  
  // TODO: I'm not really sure if this is the right spot to 'hide' the database schema
  //       maybe this should reside in catalog_queries
  
  // prepare schema creation queries
  if (not
    SqlStatement(database, 
                "CREATE TABLE IF NOT EXISTS catalog "
                "(md5path_1 INTEGER, md5path_2 INTEGER, parent_1 INTEGER, parent_2 INTEGER, inode INTEGER, "
                "hash BLOB, size INTEGER, mode INTEGER, mtime INTEGER, flags INTEGER, name TEXT, symlink TEXT, "
                "CONSTRAINT pk_catalog PRIMARY KEY (md5path_1, md5path_2));").Execute()) goto out_fail;
                
  if (not
    SqlStatement(database,
                "CREATE INDEX IF NOT EXISTS idx_catalog_parent "
                "ON catalog (parent_1, parent_2);").Execute()) goto out_fail;
  
  // TODO: wonder if this is actually still needed... inode field now holds stuff, not worth indexing
  if (not   
    SqlStatement(database,
                "CREATE INDEX IF NOT EXISTS idx_catalog_inode "
                "ON catalog (inode);").Execute()) goto out_fail;
                
  if (not
    SqlStatement(database,
                "CREATE TABLE IF NOT EXISTS properties "
                "(key TEXT, value TEXT, CONSTRAINT pk_properties PRIMARY KEY (key));").Execute()) goto out_fail;
                
  if (not
    SqlStatement(database,
                "CREATE TABLE IF NOT EXISTS nested_catalogs "
                "(path TEXT, sha1 TEXT, CONSTRAINT pk_nested_catalogs PRIMARY KEY (path));").Execute()) goto out_fail;
  
  if (not
    SqlStatement(database,
                "INSERT OR IGNORE INTO properties (key, value) VALUES ('revision', 0);").Execute()) goto out_fail;
  
  if (not
    SqlStatement(database,
                "INSERT OR REPLACE INTO properties (key, value) VALUES ('schema', '1.2');").Execute()) goto out_fail;

  sqlite3_close(database);
  return true;
  
out_fail:
  sqlite3_close(database);
  return false;
}

WritableCatalog::WritableCatalog(const string &path, Catalog *parent) :
  Catalog(path, parent)
{}

WritableCatalog::~WritableCatalog() {
  // CAUTION HOT!
  // (see Catalog.h - near the definition of FinalizePreparedStatements)
  FinalizePreparedStatements();
}

void WritableCatalog::InitPreparedStatements() {
  Catalog::InitPreparedStatements(); // polymorphism: up call
  
  insert_statement_                = new InsertDirectoryEntrySqlStatement(database());
  touch_statement_                 = new TouchSqlStatement(database());
  unlink_statement_                = new UnlinkSqlStatement(database());
  update_statement_                = new UpdateDirectoryEntrySqlStatement(database());
  max_hardlink_group_id_statement_ = new GetMaximalHardlinkGroupIdStatement(database());
}

void WritableCatalog::FinalizePreparedStatements() {
  // no polymorphism: no up call (see Catalog.h - near the definition of this method)
  
  delete insert_statement_;
  delete touch_statement_;
  delete unlink_statement_;
  delete update_statement_;
  delete max_hardlink_group_id_statement_;
}

int WritableCatalog::GetMaximalHardlinkGroupId() const {
  int result = -1;
  
  if (max_hardlink_group_id_statement_->FetchRow()) {
    result = max_hardlink_group_id_statement_->GetMaximalGroupId();
  }
  max_hardlink_group_id_statement_->Reset();
  
  return result;
}

bool WritableCatalog::CheckForExistanceAndAddEntry(const DirectoryEntry &entry, 
                                                   const string &entry_path, 
                                                   const string &parent_path) {
  // check if entry already exists
  hash::t_md5 path_hash(entry_path);
  if (Lookup(path_hash)) {
    pmesg(D_CATALOG, "entry '%s' exists and thus cannot be created", entry_path.c_str());
    return false;
  }
  
  // add the entry to the catalog
  hash::t_md5 parent_hash(parent_path);
  if (not AddEntry(entry, path_hash, parent_hash)) {
    pmesg(D_CATALOG, "something went wrong while inserting new entry '%s'", entry_path.c_str());
    return false;
  }
  
  return true;
}

bool WritableCatalog::AddEntry(const DirectoryEntry &entry, 
                               const hash::t_md5 &path_hash, 
                               const hash::t_md5 &parent_hash) {
  SetDirty();
  
  // perform a add operation for the given directory entry
  bool result = (
    insert_statement_->BindPathHash(path_hash) &&
    insert_statement_->BindParentPathHash(parent_hash) &&
    insert_statement_->BindDirectoryEntry(entry) &&
    insert_statement_->Execute()
  );
  
  insert_statement_->Reset();
  
  return result;
}

bool WritableCatalog::TouchEntry(const string &entry_path, const time_t timestamp) {
  SetDirty();

  // perform a touch operation for the given path
  hash::t_md5 path_hash(entry_path);
  bool result = (
    touch_statement_->BindPathHash(path_hash) &&
    touch_statement_->BindTimestamp(timestamp) &&
    touch_statement_->Execute()
  );
  touch_statement_->Reset();

  return result;
}

bool WritableCatalog::RemoveEntry(const string &file_path) {
  SetDirty();
  
  // perform a delete operation for the given path
  hash::t_md5 path_hash(file_path);
  bool result = (
    unlink_statement_->BindPathHash(path_hash) &&
    unlink_statement_->Execute()
  );
  
  unlink_statement_->Reset();
  
  return result;
}

bool WritableCatalog::UpdateEntry(const DirectoryEntry &entry, const hash::t_md5 &path_hash) {
  SetDirty();
  
  // perform the update operation
  bool result = (
    update_statement_->BindPathHash(path_hash) &&
    update_statement_->BindDirectoryEntry(entry) &&
    update_statement_->Execute()
  );
  
  update_statement_->Reset();
  
  return result;
}

bool WritableCatalog::UpdateLastModified() {
  const time_t now = time(NULL);
  ostringstream sql;
  sql << "INSERT OR REPLACE INTO properties "
     "(key, value) VALUES ('last_modified', '" << now << "');";
  return SqlStatement(database(), sql.str()).Execute();
}

bool WritableCatalog::IncrementRevision() {
  const string sql = "UPDATE properties SET value=value+1 WHERE key='revision';";
  return SqlStatement(database(), sql).Execute();
}

bool WritableCatalog::SetPreviousRevision(const hash::t_sha1 &hash) {
  ostringstream sql;
  sql << "INSERT OR REPLACE INTO properties "
         "(key, value) VALUES ('previous_revision', '" << hash.to_string() << "');";
   return SqlStatement(database(), sql.str()).Execute();
}

bool WritableCatalog::SplitContentIntoNewNestedCatalog(WritableCatalog *new_nested_catalog) {
  // create connection between parent and child catalogs
  if (not MakeNestedCatalogMountpoint(new_nested_catalog->path())) {
    pmesg(D_CATALOG, "failed to create nested catalog mountpoint in catalog '%s'", path().c_str());
    return false;
  }
  if (not new_nested_catalog->MakeNestedCatalogRootEntry()) {
    pmesg(D_CATALOG, "failed to create nested catalog root entry in new nested catalog '%s'", new_nested_catalog->path().c_str());
    return false;
  }
  
  // move the present directory tree into the newly created nested catalog
  // if we hit nested catalog mountpoints on the way, we return them through
  // the passed list
  list<string> nestedNestedCatalogMountpoints;
  if (not MoveDirectoryStructureToNewNestedCatalog(new_nested_catalog->path(), 
                                                   new_nested_catalog,
                                                   nestedNestedCatalogMountpoints)) {
    pmesg(D_CATALOG, "failed to move directory structure in '%s' to new nested catalog", new_nested_catalog->path().c_str());
    return false;
  }
  
  // nested catalog mountpoints found in the moved directory structure are now
  // links to nested catalogs of the newly created nested catalog.
  // move these references into the new nested catalog
  if (not MoveNestedCatalogReferencesToNewNestedCatalog(nestedNestedCatalogMountpoints, new_nested_catalog)) {
    pmesg(D_CATALOG, "failed to move nested catalog references into new nested catalog '%s'", new_nested_catalog->path().c_str());
    return false;
  }
  
  return true;
}

bool WritableCatalog::MakeNestedCatalogMountpoint(const string &mountpoint) {
  // find the directory entry to edit
  DirectoryEntry mnt_pnt_entry;
  if (not Lookup(mountpoint, &mnt_pnt_entry)) {
    return false;
  }
  
  // sanity check
  if (not mnt_pnt_entry.IsDirectory() || mnt_pnt_entry.IsNestedCatalogRoot()) {
    return false;
  }
  
  // mark this entry as nested catalog mountpoint
  mnt_pnt_entry.set_is_nested_catalog_mountpoint(true);
  
  // write back
  if (not UpdateEntry(mnt_pnt_entry, mountpoint)) {
    return false;
  }
  
  return true;
}

bool WritableCatalog::MakeNestedCatalogRootEntry() {
  // retrieve the root entry of this catalog
  DirectoryEntry root_entry;
  if (not GetRootEntry(&root_entry)) {
    pmesg(D_CATALOG, "no root entry found in catalog '%s'", path().c_str());
    return false;
  }
  
  // sanity checks
  if (not root_entry.IsDirectory() || root_entry.IsNestedCatalogMountpoint()) {
    pmesg(D_CATALOG, "root entry is not feasible for nested catalog '%s'", path().c_str());
    return false;
  }
  
  // mark this as a nested catalog root entry
  root_entry.set_is_nested_catalog_root(true);
  
  // write back
  if (not UpdateEntry(root_entry, path())) {
    return false;
  }
  
  return true;
}

bool WritableCatalog::MoveDirectoryStructureToNewNestedCatalogRecursively(const string dir_structure_root,
                                                                          WritableCatalog *new_nested_catalog,
                                                                          list<string> &nested_catalog_mountpoints) {
  // after creating a new nested catalog we have move all elements
  // now contained by the new one... list and move them recursively
  DirectoryEntryList listing;
  if (not Listing(dir_structure_root, &listing)) {
    return false;
  }
  
  // go through the listing
  DirectoryEntryList::const_iterator i,iend;
  string full_path;
  for (i = listing.begin(), iend = listing.end(); i != iend; ++i) {
    full_path = dir_structure_root + "/" + i->name();
    
    // the entries are first inserted into the new catalog
    if (not new_nested_catalog->AddEntry(*i, full_path)) {
      return false;
    }
    
    // then we check if we have some special cases:
    if (i->IsNestedCatalogMountpoint()) {
      // nested catalogs mountpoints will be return through the variable and
      // then processed later on
      nested_catalog_mountpoints.push_back(full_path);
    } else if (i->IsDirectory()) {
      // recurse deeper into the catalog structure
      if (not MoveDirectoryStructureToNewNestedCatalogRecursively(full_path,
                                                                  new_nested_catalog,
                                                                  nested_catalog_mountpoints)) {
        return false;
      }
    }
    
    // after everything is done we delete the entry from the current catalog
    if (not RemoveEntry(full_path)) {
      return false;
    }
  }
  
  // all done...
  return true;
}
bool WritableCatalog::MoveNestedCatalogReferencesToNewNestedCatalog(const list<string> &nested_catalog_references,
                                                                    WritableCatalog *new_nested_catalog) {
  list<string>::const_iterator i,iend;
  for (i = nested_catalog_references.begin(), iend = nested_catalog_references.end(); i != iend; ++i) {
    Catalog *attached_reference = NULL;
    if (not this->RemoveNestedCatalogReference(*i, &attached_reference)) {
      return false;
    }
    
    if (not new_nested_catalog->InsertNestedCatalogReference(*i, attached_reference)) {
      return false;
    }
  }
  
  return true;
}

bool WritableCatalog::InsertNestedCatalogReference(const string &mountpoint, 
                                                   Catalog *attached_reference,
                                                   const hash::t_sha1 content_hash) {
  const string sha1_string = (not content_hash.is_null()) ? content_hash.to_string() : "";
  
  // doing the SQL statement
  SqlStatement stmt(database(), "INSERT INTO nested_catalogs (path, sha1) VALUES (:p, :sha1);");
  bool successful = (
    stmt.BindText(1, mountpoint) &&
    stmt.BindText(2, sha1_string) &&
    stmt.Execute()
  );
  
  // if we got passed a reference of the in-memory object of the newly referenced
  // catalog, we add this to our own children
  if (successful && attached_reference != NULL) {
    this->AddChild(attached_reference);
  }
  
  return successful;
}

bool WritableCatalog::RemoveNestedCatalogReference(const string &mountpoint, Catalog **attached_reference) {
  // remove the nested catalog from the database
  SqlStatement stmt(database(), "DELETE FROM nested_catalogs WHERE path = :p;");
  bool successful = (
    stmt.BindText(1, mountpoint) &&
    stmt.Execute()
  );
  
  // if the reference was successfully deleted, we also have to check,
  // if there is also an attached reference in our in-memory data.
  // in this case we remove the child and return it through **attached_reference
  if (successful) {
    Catalog *child = FindChildWithMountpoint(mountpoint);
    if (child != NULL) this->RemoveChild(child);
    if (attached_reference != NULL) *attached_reference = child;
  }
  
  return successful;
}

bool WritableCatalog::UpdateNestedCatalogLink(const string &path,
                                              const hash::t_sha1 &hash) {
  const string sql = "UPDATE nested_catalogs SET sha1 = :sha1 WHERE path = :path;";
  SqlStatement stmt(database(), sql);

  stmt.BindText(1, hash.to_string());
  stmt.BindText(2, path);

  return stmt.Execute();
}

bool WritableCatalog::MergeIntoParentCatalog() const {
  // check if we deal with a nested catalog
  // otherwise we will not find a parent to merge into
  assert(not IsRoot());
  
  WritableCatalog *parent = GetWritableParent();
  
  // copy over all DirectoryEntries to the parent catalog
  if (not CopyDirectoryEntriesToParentCatalog()) {
    pmesg(D_CATALOG, "failed to copy directory entries from '%s' to parent '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }
  
  // copy the nested catalog references
  if (not CopyNestedCatalogReferencesToParentCatalog()) {
    pmesg(D_CATALOG, "failed to merge nested catalog references from '%s' to parent '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }

  // remove the nested catalog reference for this nested catalog
  // CAUTION! from now on this catalog will be dangling!
  if (not parent->RemoveNestedCatalogReference(this->path())) {
    pmesg(D_CATALOG, "failed to remove nested catalog reference '%s', in parent catalog '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }
  
  return true;
}

bool WritableCatalog::CopyNestedCatalogReferencesToParentCatalog() const {
  WritableCatalog *parent = GetWritableParent();
  
  // obtain a list of all nested catalog references
  NestedCatalogReferenceList nested_catalog_references = ListNestedCatalogReferences();
  
  // go through the list and update the databases
  // simultaneously we are checking if the referenced catalogs are currently
  // attached and update the in-memory data structures as well
  NestedCatalogReferenceList::const_iterator i,iend;
  for (i = nested_catalog_references.begin(), 
       iend = nested_catalog_references.end();
       i != iend;
       ++i) {
    Catalog *child = FindChildWithMountpoint(i->path);
    parent->InsertNestedCatalogReference(i->path, child, i->content_hash);
  }
  
  return true;
}

bool WritableCatalog::CopyDirectoryEntriesToParentCatalog() const {
  // we can simply copy all entries from this database to the 'other' database
  // BUT: 1. this would create collisions in hardlink group IDs.
  //         therefor we first update all hardlink group IDs to fit behind the
  //         ones in the 'other' database
  //      2. the root entry of the nested catalog is present twice:
  //         1. in the parent directory (as mount point) and
  //         2. in the nested catalog (as root entry)
  //         therefore we delete the mount point from the parent before merging
  
  WritableCatalog *parent = GetWritableParent();
  
  // Update hardlink group IDs in this nested catalog.
  // To avoid collisions we add the maximal present hardlink group ID in parent
  // to all hardlink group IDs in the nested catalog.
  // (CAUTION: hardlink group ID is saved in the inode field --> legacy :-) )
  const int offset = parent->GetMaximalHardlinkGroupId();
  stringstream ss;
  ss << "UPDATE catalog SET inode = inode + " << offset << " WHERE inode > 0;";
  const string update_hardlink_group_ids = ss.str();
  
  SqlStatement update_hardlink_groups(database(), update_hardlink_group_ids);
  if (not update_hardlink_groups.Execute()) {
    pmesg(D_CATALOG, "failed to harmonize the hardlink group IDs in '%s'", this->path().c_str());
    return false;
  }
  
  // remove the nested catalog mount point
  // it will be replaced with the nested catalog root entry when copying
  if (not parent->RemoveEntry(this->path())) {
    pmesg(D_CATALOG, "failed to remove mount point '%s' of nested catalog to be merged", this->path().c_str());
    return false;
  }
  
  // now copy over all DirectoryEntries to the 'other' catalog
  // there will be no data collisions, as we resolved them before hands
  if (not SqlStatement(database(), "ATTACH '" + parent->database_file() + "' AS other;").Execute()) {
    pmesg(D_CATALOG, "failed to attach database of catalog '%s' in catalog '%s'", parent->path().c_str(), this->path().c_str());
    return false;
  }
  if (not SqlStatement(database(), "INSERT INTO other.catalog "
                                   "SELECT * FROM main.catalog;").Execute()) {
    pmesg(D_CATALOG, "failed to copy DirectoryEntries from catalog '%s' to catalog '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }
  if (not SqlStatement(database(), "DETACH other;").Execute()) {
    pmesg(D_CATALOG, "failed to detach database of catalog '%s' from catalog '%s'", parent->path().c_str(), this->path().c_str());
    return false;
  }
  
  // change the just copied nested catalog root to an ordinary directory
  // (the nested catalog is merged into it's parent)
  DirectoryEntry old_root_entry;
  if (not parent->Lookup(this->path(), &old_root_entry)) {
    pmesg(D_CATALOG, "root entry of removed nested catalog '%s' not found in parent catalog '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }
  
  // sanity checks
  // just see if everything worked out with our copied 'root entry'
  if (not old_root_entry.IsDirectory() || 
      not old_root_entry.IsNestedCatalogRoot() ||
      old_root_entry.IsNestedCatalogMountpoint()) {
    pmesg(D_CATALOG, "former root entry '%s' looks strange in '%s'", this->path().c_str(), parent->path().c_str());
    return false;
  }
  
  // remove the nested catalog root mark
  old_root_entry.set_is_nested_catalog_root(false);
  if (not parent->UpdateEntry(old_root_entry, this->path())) {
    pmesg(D_CATALOG, "unable to remove the 'nested catalog root' mark from '%s'", this->path().c_str());
    return false;
  }
  
  return true;
}

}