/**
 * Webpack config for production electron main process
 */

import { EsbuildPlugin } from "esbuild-loader";
import { fdir } from "fdir";
import path from "path";
import webpack from "webpack";
import { BundleAnalyzerPlugin } from "webpack-bundle-analyzer";
import { merge } from "webpack-merge";

import checkNodeEnv from "../scripts/check-node-env";
import deleteSourceMaps from "../scripts/delete-source-maps";
import baseConfig from "./webpack.config.base";
import webpackPaths from "./webpack.paths";

checkNodeEnv("production");
deleteSourceMaps();

function resolveWorkers(rootFolder: string): Record<string, string> {
  const workers: Record<string, string> = {};
  // eslint-disable-next-line new-cap
  const crawler = new fdir().glob("./**/*.worker.ts").withFullPaths();
  const files = crawler.crawl(rootFolder).sync() as string[];
  files.forEach((filename) => {
    const basename = path.basename(filename, ".ts");
    workers[basename] = filename;
  });
  return workers;
}

/**
 * Electron starts its entry script (package.json `main`) through Node's ES module loader, which
 * reads the whole file to guess its module format and then scans it for named exports: about
 * 70 ms of every start for the 2.4 MB bundle on a fast PC, more on a slow one. So the bundle is
 * app.js and main.js is two lines that require() it: require() compiles it as CommonJS directly.
 */
class EntryStubPlugin {
  apply(compiler: webpack.Compiler) {
    compiler.hooks.thisCompilation.tap("EntryStubPlugin", (compilation) => {
      compilation.hooks.processAssets.tap(
        { name: "EntryStubPlugin", stage: webpack.Compilation.PROCESS_ASSETS_STAGE_ADDITIONAL },
        () => {
          compilation.emitAsset("main.js", new webpack.sources.RawSource('"use strict";\nrequire("./app.js");\n'));
        },
      );
    });
  }
}

const devtoolsConfig =
  process.env.DEBUG_PROD === "true"
    ? {
        devtool: "source-map",
      }
    : {};

const configuration: webpack.Configuration = {
  ...devtoolsConfig,

  mode: "production",

  target: "electron-main",

  entry: {
    // Started by main.js (EntryStubPlugin).
    app: path.join(webpackPaths.srcMainPath, "main.ts"),
    preload: path.join(webpackPaths.srcMainPath, "preload.ts"),
    ...resolveWorkers(webpackPaths.srcPath),
  },

  output: {
    path: webpackPaths.distMainPath,
    filename: "[name].js",
  },

  optimization: {
    /**
     * Extract shared dependencies into a common chunk to avoid duplication
     * across entry points (main + 4 workers). Without this, @slippi/slippi-js
     * and its transitive deps (e.g. enet) are bundled independently into each
     * entry point that imports them, wasting ~1-2MB total.
     *
     * Externalizing these in webpack.config.base.ts isn't viable because they
     * are root package.json deps — they don't exist in release/app/node_modules/
     * at runtime. splitChunks resolves the duplication at build time instead,
     * producing tree-shaken, minified shared chunks in dist/main/.
     */
    splitChunks: {
      chunks: "all",
      minSize: 50000,
      minChunks: 2,
      cacheGroups: {
        defaultVendors: {
          test: /[\\/]node_modules[\\/]/,
          priority: -10,
          reuseExistingChunk: true,
        },
      },
    },
    minimizer: [
      new EsbuildPlugin({
        target: "es2022",
      }),
    ],
  },

  plugins: [
    new EntryStubPlugin(),

    new BundleAnalyzerPlugin({
      analyzerMode: process.env.ANALYZE === "true" ? "server" : "disabled",
    }),

    /**
     * Create global constants which can be configured at compile time.
     *
     * Useful for allowing different behaviour between development builds and
     * release builds
     *
     * NODE_ENV should be production so that modules do not perform certain
     * development checks
     */
    new webpack.EnvironmentPlugin({
      NODE_ENV: "production",
      DEBUG_PROD: false,
      START_MINIMIZED: false,
    }),
  ],

  /**
   * Disables webpack processing of __dirname and __filename.
   * If you run the bundle in node.js it falls back to these values of node.js.
   * https://github.com/webpack/webpack/issues/2010
   */
  node: {
    __dirname: false,
    __filename: false,
  },
};

export default merge(baseConfig, configuration);
