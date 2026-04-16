# Market Research Step 5: Competitive Analysis

## MANDATORY EXECUTION RULES (READ FIRST):

- 🛑 NEVER generate content without web search verification

- 📖 CRITICAL: ALWAYS read the complete step file before taking any action - partial understanding leads to incomplete decisions
- 🔄 CRITICAL: When loading next step with 'C', ensure the entire file is read and understood before proceeding
- ✅ Search the web to verify and supplement your knowledge with current facts
- 📋 YOU ARE A COMPETITIVE ANALYST, not content generator
- 💬 FOCUS on competitive landscape and market positioning
- 🔍 WEB SEARCH REQUIRED - verify current facts against live sources
- ✅ YOU MUST ALWAYS SPEAK OUTPUT In your Agent communication style with the config `{communication_language}`

## EXECUTION PROTOCOLS:

- 🎯 Show web search analysis before presenting findings
- ⚠️ Present [C] complete option after competitive analysis content generation
- 💾 ONLY save when user chooses C (Complete)
- 📖 Update frontmatter `stepsCompleted: [1, 2, 3, 4, 5]` before completing workflow
- 🚫 FORBIDDEN to complete workflow until C is selected

## CONTEXT BOUNDARIES:

- Current document and frontmatter from previous steps are available
- Focus on competitive landscape and market positioning analysis
- Web search capabilities with source verification are enabled
- May need to search for specific competitor information

## YOUR TASK:

Conduct comprehensive competitive analysis with emphasis on market positioning.

## COMPETITIVE ANALYSIS SEQUENCE:

### 1. Begin Competitive Analysis

Start with competitive research approach:
"Now I'll conduct **competitive analysis** to understand the competitive landscape.

**Competitive Analysis Focus:**

- Key players and market share
- Competitive positioning strategies
- Strengths and weaknesses analysis
- Market differentiation opportunities
- Competitive threats and challenges

**Let me search for current competitive information.**"

### 2. Parallel Competitive Research Execution

**Execute multiple web searches simultaneously:**

Search the web: "{{research_topic}} key market players market share"
Search the web: "{{research_topic}} competitive positioning strategies"
Search the web: "{{research_topic}} competitor strengths weaknesses"
Search the web: "{{research_topic}} market differentiation opportunities competitors"

**Analysis approach:**

- Look for recent market reports, competitive intelligence, and industry analyses
- Use competitor sites, annual reports, analyst coverage, and trade publications
- Verify market share, positioning claims, and notable strategic moves with current sources
- Identify both direct competitors and adjacent alternatives shaping customer expectations

### 3. Analyze and Aggregate Results

**Collect and analyze findings from all parallel searches:**

"After executing comprehensive competitive web searches, let me analyze and aggregate the findings:

**Research Coverage:**

- Key players and market share
- Competitive positioning and differentiation
- Strengths, weaknesses, threats, and opportunity patterns

**Cross-Competitive Analysis:**
[Identify patterns connecting competitor strategy, market gaps, and positioning opportunities]

**Quality Assessment:**
[Overall confidence levels and research gaps identified]"

### 4. Generate Competitive Analysis Content

Prepare competitive analysis with web search citations:

#### Content Structure:

When saving to document, append these Level 2 and Level 3 sections:

```markdown
## Competitive Landscape

### Key Market Players

[Key players analysis with market share data]
_Source: [URL]_

### Market Share Analysis

[Market share analysis with source citations]
_Source: [URL]_

### Competitive Positioning

[Positioning analysis with source citations]
_Source: [URL]_

### Strengths and Weaknesses

[SWOT analysis with source citations]
_Source: [URL]_

### Market Differentiation

[Differentiation analysis with source citations]
_Source: [URL]_

### Competitive Threats

[Threats analysis with source citations]
_Source: [URL]_

### Opportunities

[Competitive opportunities analysis with source citations]
_Source: [URL]_
```

### 5. Present Analysis and Complete Option

Show the generated competitive analysis and present complete option:
"I've completed the **competitive analysis** for the competitive landscape.

**Key Competitive Findings:**

- Key market players and market share identified
- Competitive positioning strategies mapped
- Strengths and weaknesses thoroughly analyzed
- Market differentiation opportunities identified
- Competitive threats and challenges documented

**Ready to complete the market research?**
[C] Complete Research - Save competitive analysis and proceed to research completion

**HALT — wait for user response before proceeding.**

### 6. Handle Complete Selection

#### If 'C' (Complete Research):

- Append the final content to the research document
- Update frontmatter: `stepsCompleted: [1, 2, 3, 4, 5]`
- Load: `./step-06-research-completion.md`

## APPEND TO DOCUMENT:

When user selects 'C', append the content directly to the research document using the structure from step 2.

## SUCCESS METRICS:

✅ Key market players identified
✅ Market share analysis completed with source verification
✅ Competitive positioning strategies clearly mapped
✅ Strengths and weaknesses thoroughly analyzed
✅ Market differentiation opportunities identified
✅ [C] complete option presented and handled correctly
✅ Content properly appended to document when C selected
✅ Market research workflow completed successfully

## FAILURE MODES:

❌ Relying solely on training data without web verification for current facts

❌ Missing key market players or market share data
❌ Incomplete competitive positioning analysis
❌ Not identifying market differentiation opportunities
❌ Not presenting completion option for research workflow
❌ Appending content without user selecting 'C'

❌ **CRITICAL**: Reading only partial step file - leads to incomplete understanding and poor decisions
❌ **CRITICAL**: Proceeding with 'C' without fully reading and understanding the next step file
❌ **CRITICAL**: Making decisions without complete understanding of step requirements and protocols

## COMPETITIVE RESEARCH PROTOCOLS:

- Search for industry reports and competitive intelligence
- Use competitor company websites and annual reports
- Research market research firm competitive analyses
- Note competitive advantages and disadvantages
- Search for recent market developments and disruptions

## MARKET RESEARCH COMPLETION:

When 'C' is selected:

- All market research steps completed
- Comprehensive market research document generated
- All sections appended with source citations
- Market research workflow status updated
- Final recommendations provided to user

## NEXT STEP:

After user selects 'C', load `./step-06-research-completion.md` to produce the final comprehensive market research document with strategic synthesis, executive summary, and complete document structure.
