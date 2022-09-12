//
// Created by paul on 5/27/20.
//

#ifndef TEMPLATEFS_FUSEOPERATIONS_H
#define TEMPLATEFS_FUSEOPERATIONS_H

extern const struct fuse_operations templatefsOperations;

void * initPrivateData( const char * mountPath, const char * templatePath );

#endif //TEMPLATEFS_FUSEOPERATIONS_H
