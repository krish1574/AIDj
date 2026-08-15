const { getDefaultConfig } = require('expo/metro-config');
const path = require('node:path');

const projectRoot = __dirname;
const workspaceRoot = path.resolve(projectRoot, '../..');

const config = getDefaultConfig(projectRoot);

// npm workspaces hoists dependencies to the repo root, and @ai-dj/core is
// consumed as raw TypeScript from outside the app directory. Metro has to be
// told about both or it will not resolve either.
config.watchFolders = [workspaceRoot];
config.resolver.nodeModulesPaths = [
  path.resolve(projectRoot, 'node_modules'),
  path.resolve(workspaceRoot, 'node_modules'),
];
// Without this, a hoisted copy and a local copy of react can both be loaded,
// which produces the "invalid hook call" failure that looks like a bug in your
// own code.
config.resolver.disableHierarchicalLookup = true;

// The local native module is imported as `aidj-audio` rather than by relative
// path. TypeScript resolves that through tsconfig paths, but Metro needs to be
// told separately or the import fails at runtime with "module not found".
config.resolver.extraNodeModules = {
  'aidj-audio': path.resolve(projectRoot, 'modules/aidj-audio'),
};

module.exports = config;
