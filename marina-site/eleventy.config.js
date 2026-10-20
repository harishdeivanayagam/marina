export default function (eleventyConfig) {
  // 333580 -> "333,580"
  eleventyConfig.addFilter("numberFormat", (n) => Number(n).toLocaleString("en-US"));

  // maxBy(rows, "value") -> max field value; maxOf(nums) -> max of plain numbers
  eleventyConfig.addFilter("maxBy", (rows, key) => Math.max(...rows.map((r) => r[key])));
  eleventyConfig.addFilter("minBy", (rows, key) => Math.min(...rows.map((r) => r[key])));
  eleventyConfig.addFilter("maxOf", (nums) => Math.max(...nums));
  eleventyConfig.addFilter("minOf", (nums) => Math.min(...nums));

  eleventyConfig.addPassthroughCopy({ "public": "." });

  return {
    dir: {
      input: "src",
      includes: "_includes",
      data: "_data",
      output: "_site",
    },
    templateFormats: ["html", "njk", "md"],
    htmlTemplateEngine: "njk",
    markdownTemplateEngine: "njk",
    passthroughFileCopy: true,
  };
}